#include "quickfix/Application.h"
#include "quickfix/MessageCracker.h"
#include "quickfix/FileStore.h"
#include "quickfix/FileLog.h"
#include "quickfix/SocketInitiator.h"
#include "quickfix/SessionSettings.h"
#include "quickfix/fix44/NewOrderSingle.h"
#include "quickfix/fix44/OrderCancelRequest.h"
#include "quickfix/fix44/ExecutionReport.h"
#include "quickfix/fix44/OrderCancelReject.h"
#include "quickfix/fix44/BusinessMessageReject.h"
#include <algorithm>
#include <ctime>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>

static std::string pretty(std::string s) {
    std::replace(s.begin(), s.end(), '\x01', '|');
    return s;
}

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

struct Order {
    std::string symbol;
    char side = '1';
    double qty = 0;
    double price = 0;     // 0 = market
    double cumQty = 0;
    std::string status = "Sent";
};

class ClientApp : public FIX::Application, public FIX::MessageCracker {
    std::mutex mtx_;
    FIX::SessionID sid_;
    bool loggedOn_ = false;
    std::map<std::string, Order> orders_;
    long long counter_ = 0;
    std::string prefix_ = "ORD" + std::to_string(std::time(nullptr)) + "_";

public:
    // ---- FIX::Application callbacks ----
    void onCreate(const FIX::SessionID&) override {}
    void onLogon(const FIX::SessionID& s) override {
        std::lock_guard<std::mutex> lk(mtx_);
        sid_ = s; loggedOn_ = true;
        std::cout << "\n[Logon] " << s << "\n> " << std::flush;
    }
    void onLogout(const FIX::SessionID& s) override {
        std::lock_guard<std::mutex> lk(mtx_);
        loggedOn_ = false;
        std::cout << "\n[Logout] " << s << "\n> " << std::flush;
    }
    void toAdmin(FIX::Message&, const FIX::SessionID&) override {}
    void fromAdmin(const FIX::Message&, const FIX::SessionID&) override {}
    void toApp(FIX::Message&, const FIX::SessionID&) override {}
    void fromApp(const FIX::Message& m, const FIX::SessionID& s) override { crack(m, s); }

    // ---- incoming business messages ----
    void onMessage(const FIX44::ExecutionReport& er, const FIX::SessionID&) override {
        FIX::ClOrdID cl; FIX::OrdStatus st;
        er.get(cl); er.get(st);
        FIX::CumQty cq(0);
        if (er.isSetField(FIX::FIELD::CumQty)) er.get(cq);
        {
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
    void onMessage(const FIX44::OrderCancelReject& r, const FIX::SessionID&) override {
        std::cout << "\n[CancelReject] " << pretty(r.toString()) << "\n> " << std::flush;
    }
    void onMessage(const FIX44::BusinessMessageReject& r, const FIX::SessionID&) override {
        std::cout << "\n[BusinessReject] " << pretty(r.toString()) << "\n> " << std::flush;
    }

    // ---- commands from the console ----
    void sendOrder(char side, const std::string& sym, double qty, double price) {
        std::string id;
        FIX::SessionID sid;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!loggedOn_) { std::cout << "Not logged on.\n"; return; }
            id = prefix_ + std::to_string(++counter_);
            sid = sid_;
            Order o; o.symbol = sym; o.side = side; o.qty = qty; o.price = price;
            orders_[id] = o;
        }
        // The lock is released before sending (see the note below).
        bool limit = price > 0;
        FIX44::NewOrderSingle order(
            FIX::ClOrdID(id), FIX::Side(side), FIX::TransactTime(),
            FIX::OrdType(limit ? FIX::OrdType_LIMIT : FIX::OrdType_MARKET));
        order.set(FIX::Symbol(sym));
        order.set(FIX::OrderQty(qty));
        if (limit) {
            order.set(FIX::Price(price));
            order.set(FIX::TimeInForce(FIX::TimeInForce_DAY));
        }
        FIX::Session::sendToTarget(order, sid);
        std::cout << "Sent order " << id << "\n";
    }

    void cancelOrder(const std::string& origId) {
        Order o; FIX::SessionID sid; std::string newId;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!loggedOn_) { std::cout << "Not logged on.\n"; return; }
            auto it = orders_.find(origId);
            if (it == orders_.end()) { std::cout << "Unknown order.\n"; return; }
            o = it->second; sid = sid_;
            newId = prefix_ + std::to_string(++counter_);
        }
        FIX44::OrderCancelRequest c(
            FIX::OrigClOrdID(origId), FIX::ClOrdID(newId),
            FIX::Side(o.side), FIX::TransactTime());
        c.set(FIX::Symbol(o.symbol));
        c.set(FIX::OrderQty(o.qty));
        FIX::Session::sendToTarget(c, sid);
        std::cout << "Sent cancel " << newId << " for " << origId << "\n";
    }

    void listOrders() {
        std::lock_guard<std::mutex> lk(mtx_);
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
        FIX::SessionSettings settings(argc > 1 ? argv[1] : "initiator.cfg");
        ClientApp app;
        FIX::FileStoreFactory store(settings);
        FIX::FileLogFactory log(settings);
        FIX::SocketInitiator initiator(app, store, settings, log);
        initiator.start();
        help();

        std::string line;
        while (std::cout << "> " << std::flush, std::getline(std::cin, line)) {
            std::istringstream in(line);
            std::string cmd; in >> cmd;
            if (cmd == "buy" || cmd == "sell") {
                std::string sym; double qty = 0, px = 0;
                in >> sym >> qty;
                in >> px;                       // stays 0 if missing
                if (sym.empty() || qty <= 0) { std::cout << "Usage: buy SYMBOL QTY [PRICE]\n"; continue; }
                app.sendOrder(cmd == "buy" ? '1' : '2', sym, qty, px);
            }
            else if (cmd == "cancel") {
                std::string id; in >> id;
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
        initiator.stop();
    }
    catch (std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}