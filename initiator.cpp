// ============================================================================
// initiator.cpp  -  the "client" side (a tiny OMS) of the FIX demo
//
// What it does:
//   * connects to the acceptor (the fake exchange) and logs on
//   * reads commands you type (buy / sell / cancel / orders / quit)
//   * sends FIX messages, receives ExecutionReports, tracks order state
//
// Threads (important!):
//   * main thread        : reads the console and calls sendOrder()/cancelOrder()
//   * QuickFIX thread(s) : run the callbacks (onLogon, fromApp, onMessage ...)
//   Both touch orders_, so every access to shared data is protected by mtx_.
// ============================================================================

#include "quickfix/Application.h"          // FIX::Application (the 6 callbacks)
#include "quickfix/MessageCracker.h"       // crack(): routes a message to onMessage()
#include "quickfix/FileStore.h"            // saves sequence numbers + sent messages
#include "quickfix/FileLog.h"              // writes raw messages + events to log files
#include "quickfix/SocketInitiator.h"      // the TCP client engine
#include "quickfix/SessionSettings.h"      // reads initiator.cfg
#include "quickfix/fix44/NewOrderSingle.h"        // 35=D
#include "quickfix/fix44/OrderCancelRequest.h"    // 35=F
#include "quickfix/fix44/ExecutionReport.h"       // 35=8
#include "quickfix/fix44/OrderCancelReject.h"     // 35=9
#include "quickfix/fix44/BusinessMessageReject.h" // 35=j
#include <algorithm>
#include <ctime>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>

// FIX fields are separated by an invisible SOH character (0x01).
// Replace it with '|' so a message is readable on screen.
static std::string pretty(std::string s) {
    std::replace(s.begin(), s.end(), '\x01', '|');
    return s;
}

// Tag 39 (OrdStatus) is a single character. Turn it into a readable word.
static const char* statusText(char st) {
    switch (st) {
    case '0': return "New";
    case '1': return "PartiallyFilled";
    case '2': return "Filled";
    case '4': return "Canceled";
    case '5': return "Replaced";
    case '6': return "PendingCancel";
    case '8': return "Rejected";
    case 'A': return "PendingNew";
    case 'E': return "PendingReplace";
    default:  return "?";
    }
}

// Our own record of an order (the "order store" of this mini OMS).
struct Order {
    std::string symbol;
    char   side = '1';     // '1' = Buy, '2' = Sell (FIX tag 54)
    double qty = 0;       // tag 38
    double price = 0;       // tag 44, 0 means market order
    double cumQty = 0;       // tag 14, total filled so far
    std::string status = "Sent";
};

class ClientApp : public FIX::Application, public FIX::MessageCracker {
    std::mutex mtx_;                        // protects everything below
    FIX::SessionID sid_;                    // which FIX session to send on
    bool loggedOn_ = false;                 // true between onLogon and onLogout
    std::map<std::string, Order> orders_;   // key = ClOrdID (tag 11)
    long long counter_ = 0;                 // numbers our orders 1, 2, 3 ...
    // ClOrdIDs must be unique. Starting time in the prefix means a restarted
    // program never reuses "ORD..._1" from an earlier run.
    std::string prefix_ = "ORD" + std::to_string(std::time(nullptr)) + "_";
    const FIX::SessionSettings& settings_;

public:
    explicit ClientApp(const FIX::SessionSettings& s) : settings_(s) {}

    // ------------------------------------------------------------------
    // FIX::Application: six callbacks. QuickFIX calls these, on ITS threads.
    // ------------------------------------------------------------------

    // Session object created at startup (before any connection).
    void onCreate(const FIX::SessionID&) override {}

    // Logon handshake finished: the session is ready to send orders.
    void onLogon(const FIX::SessionID& s) override {
        std::lock_guard<std::mutex> lk(mtx_);
        sid_ = s;
        loggedOn_ = true;
        std::cout << "\n[Logon] " << s << "\n> " << std::flush;
    }

    // Session ended (logout or lost connection).
    void onLogout(const FIX::SessionID& s) override {
        std::lock_guard<std::mutex> lk(mtx_);
        loggedOn_ = false;
        std::cout << "\n[Logout] " << s << "\n> " << std::flush;
    }

    // Admin messages (Logon, Heartbeat, Logout ...).
    // toAdmin  = about to be sent. Day 3 adds Username/Password here.
    // fromAdmin = just received. A real acceptor validates credentials here.
    void toAdmin(FIX::Message& message, const FIX::SessionID& sid) override {
        // Called just before an admin message is sent. We only care about Logon.
        FIX::MsgType msgType;
        message.getHeader().getField(msgType);
        if (msgType.getValue() == FIX::MsgType_Logon) {
            const FIX::Dictionary& d = settings_.get(sid);   // this session's settings
            message.setField(FIX::Username(d.getString("Username")));  // 553
            message.setField(FIX::Password(d.getString("Password")));  // 554
        }
    }

    void fromAdmin(const FIX::Message&, const FIX::SessionID&) override {}

    // Business messages (orders, fills ...).
    // toApp = about to be sent. A real OMS can veto stale orders here.
    // fromApp = just received: hand over to crack(), which looks at tag 35
    //           and calls the matching onMessage(...) below.
    void toApp(FIX::Message&, const FIX::SessionID&) override {}
    void fromApp(const FIX::Message& m, const FIX::SessionID& s) override { crack(m, s); }

    // ------------------------------------------------------------------
    // Incoming business messages (called by crack(), on a QuickFIX thread)
    // ------------------------------------------------------------------

    // 35=8 ExecutionReport: the venue tells us what happened to an order.
    void onMessage(const FIX44::ExecutionReport& er, const FIX::SessionID&) override {
        FIX::ClOrdID cl;        // tag 11: which of OUR orders this is about
        FIX::OrdStatus st;      // tag 39: the order's state now
        er.get(cl);             // get() throws if the tag is missing
        er.get(st);
        FIX::CumQty cq(0);      // tag 14 (optional here, default 0)
        if (er.isSetField(FIX::FIELD::CumQty)) er.get(cq);
        {
            // Update our order store. The lock is held only for this block.
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = orders_.find(cl.getValue());
            if (it != orders_.end()) {
                it->second.status = statusText(st.getValue());
                it->second.cumQty = cq.getValue();
            }
        }
        std::cout << "\n[ExecReport] " << cl.getValue() << " -> " << statusText(st.getValue())
            << " cumQty=" << cq.getValue() << "\n  " << pretty(er.toString()) << "\n> " << std::flush;
    }

    // 35=9 OrderCancelReject: the venue refused our cancel/replace.
    void onMessage(const FIX44::OrderCancelReject& r, const FIX::SessionID&) override {
        std::cout << "\n[CancelReject] " << pretty(r.toString()) << "\n> " << std::flush;
    }

    // 35=j BusinessMessageReject: the venue understood the format but refuses
    // the function (e.g. "Unsupported Message Type"). We MUST handle it:
    // with no handler, crack() throws and QuickFIX answers with another
    // reject, which can start a reject ping-pong between the two sides.
    void onMessage(const FIX44::BusinessMessageReject& r, const FIX::SessionID&) override {
        std::cout << "\n[BusinessReject] " << pretty(r.toString()) << "\n> " << std::flush;
    }

    // ------------------------------------------------------------------
    // Commands called from main() (main thread)
    // ------------------------------------------------------------------

    void sendOrder(char side, const std::string& sym, double qty, double price) {
        std::string id;
        FIX::SessionID sid;
        {   // ---- locked section: only touch OUR data, then release ----
            // The braces matter: lock_guard unlocks when the block ends.
            std::lock_guard<std::mutex> lk(mtx_);
            if (!loggedOn_) { std::cout << "Not logged on.\n"; return; }
            id = prefix_ + std::to_string(++counter_);   // new unique ClOrdID
            sid = sid_;                                  // copy the session id
            Order o; o.symbol = sym; o.side = side; o.qty = qty; o.price = price;
            orders_[id] = o;                             // remember the order
        }   // ---- mutex released here ----

        // We send WITHOUT holding our mutex. If we held it while sending,
        // a QuickFIX thread (which holds the session's own lock) could be
        // waiting for our mutex in onMessage while we wait for the session
        // lock in sendToTarget: both stuck forever (deadlock).
        bool limit = price > 0;
        FIX44::NewOrderSingle order(
            FIX::ClOrdID(id),                 // 11
            FIX::Side(side),                  // 54
            FIX::TransactTime(),              // 60 (now)
            FIX::OrdType(limit ? FIX::OrdType_LIMIT : FIX::OrdType_MARKET)); // 40
        order.set(FIX::Symbol(sym));          // 55
        order.set(FIX::OrderQty(qty));        // 38
        if (limit) {
            order.set(FIX::Price(price));                       // 44
            order.set(FIX::TimeInForce(FIX::TimeInForce_DAY));  // 59
        }
        // QuickFIX adds the header (34, 49, 52, 56), length (9) and checksum (10),
        // stores the message, and writes it to the socket.
        FIX::Session::sendToTarget(order, sid);
        std::cout << "Sent order " << id << "\n";
    }

    void cancelOrder(const std::string& origId) {
        Order o;
        FIX::SessionID sid;
        std::string newId;
        {   // locked: look up the order and reserve a new ClOrdID
            std::lock_guard<std::mutex> lk(mtx_);
            if (!loggedOn_) { std::cout << "Not logged on.\n"; return; }
            auto it = orders_.find(origId);
            if (it == orders_.end()) { std::cout << "Unknown order.\n"; return; }
            o = it->second;                  // copy, so we can use it unlocked
            sid = sid_;
            newId = prefix_ + std::to_string(++counter_);
        }
        // A cancel is its own message with its own ClOrdID (11),
        // pointing at the order to cancel with OrigClOrdID (41).
        FIX44::OrderCancelRequest c(
            FIX::OrigClOrdID(origId),
            FIX::ClOrdID(newId),
            FIX::Side(o.side),
            FIX::TransactTime());
        c.set(FIX::Symbol(o.symbol));        // the FIX 4.4 dictionary requires 55 ...
        c.set(FIX::OrderQty(o.qty));         // ... and 38 on a cancel request
        FIX::Session::sendToTarget(c, sid);
        std::cout << "Sent cancel " << newId << " for " << origId << "\n";
    }

    void listOrders() {
        std::lock_guard<std::mutex> lk(mtx_);   // we read orders_, so we lock
        if (orders_.empty()) { std::cout << "No orders.\n"; return; }
        for (auto& [id, o] : orders_)
            std::cout << id << "  " << (o.side == '1' ? "BUY " : "SELL") << " " << o.symbol
            << " qty=" << o.qty << " px=" << o.price
            << " cum=" << o.cumQty << "  " << o.status << "\n";
    }
};

static void help() {
    std::cout << "Commands:\n"
        "  buy  SYMBOL QTY [PRICE]   (no price = market order)\n"
        "  sell SYMBOL QTY [PRICE]\n"
        "  cancel CLORDID\n"
        "  orders\n"
        "  help\n"
        "  quit\n";
}

int main(int argc, char** argv) {
    try {
        // 1. Read the config file (initiator.cfg unless another path is given).
        FIX::SessionSettings settings(argc > 1 ? argv[1] : "initiator.cfg");

        // 2. Our application object (the callbacks above).
        //ClientApp app;
        ClientApp app(settings);

        // 3. Factories: where to keep sequence numbers/messages and logs.
        //    They read FileStorePath / FileLogPath from the settings.
        FIX::FileStoreFactory store(settings);
        FIX::FileLogFactory log(settings);

        // 4. The engine. In QuickFIX 1.16 the order is
        //    (application, storeFactory, settings, logFactory).
        FIX::SocketInitiator initiator(app, store, settings, log);

        // 5. start() returns at once. QuickFIX starts its own threads that
        //    connect, send Logon, exchange Heartbeats, and call our callbacks.
        initiator.start();
        help();

        // 6. The main thread only reads the console and calls our methods.
        std::string line;
        while (std::cout << "> " << std::flush, std::getline(std::cin, line)) {
            std::istringstream in(line);
            std::string cmd;
            in >> cmd;
            if (cmd == "buy" || cmd == "sell") {
                std::string sym;
                double qty = 0, px = 0;
                in >> sym >> qty;
                in >> px;                         // stays 0 if not typed
                if (sym.empty() || qty <= 0) { std::cout << "Usage: buy SYMBOL QTY [PRICE]\n"; continue; }
                app.sendOrder(cmd == "buy" ? '1' : '2', sym, qty, px);
            }
            else if (cmd == "cancel") {
                std::string id;
                in >> id;
                if (id.empty()) { std::cout << "Usage: cancel CLORDID\n"; continue; }
                app.cancelOrder(id);
            }
            else if (cmd == "orders") {
                app.listOrders();
            }
            else if (cmd == "help") {
                help();
            }
            else if (cmd == "quit") {
                break;
            }
            else if (!cmd.empty()) {
                std::cout << "Unknown command. Type help.\n";
            }
        }

        // 7. Clean shutdown: sends Logout (35=5) and closes the connection.
        initiator.stop();
    }
    catch (std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}