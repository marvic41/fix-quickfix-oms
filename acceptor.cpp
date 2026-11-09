// ============================================================================
// acceptor.cpp  -  the "exchange" side (a simulator) of the FIX demo
//
// What it does:
//   * listens on the port from acceptor.cfg and accepts the initiator's logon
//   * receives a NewOrderSingle (35=D) and immediately fills it completely
//     by sending an ExecutionReport (35=8)
//
// What it does NOT do yet (planned for Week 2):
//   * cancel (35=F) / replace (35=G): QuickFIX answers with a 35=j reject
//   * partial fills, rejects, duplicate-ClOrdID checks, market prices
//   * persistent OrderID/ExecID (the counter restarts at 1 on every run)
// ============================================================================

#include "quickfix/Application.h"
#include "quickfix/MessageCracker.h"
#include "quickfix/FileStore.h"
#include "quickfix/FileLog.h"
#include "quickfix/SocketAcceptor.h"       // the TCP server engine
#include "quickfix/SessionSettings.h"
#include "quickfix/fix44/NewOrderSingle.h"
#include "quickfix/fix44/ExecutionReport.h"
#include <iostream>
#include <string>

class ExecApp : public FIX::Application, public FIX::MessageCracker {
    int id_ = 0;    // used to make OrderID/ExecID. Only QuickFIX's thread(s)
    const FIX::SessionSettings& settings_;

public:
    explicit ExecApp(const FIX::SessionSettings& s) : settings_(s) {}

    // touch it here; with several sessions you would need a lock.
    // ------------------------------------------------------------------
    // The six FIX::Application callbacks (all called on QuickFIX's threads)
    // ------------------------------------------------------------------
    void onCreate(const FIX::SessionID&) override {}                       // session object created
    void onLogon(const FIX::SessionID& s) override { std::cout << "Logon: " << s << "\n"; }
    void onLogout(const FIX::SessionID& s) override { std::cout << "Logout: " << s << "\n"; }
    void toAdmin(FIX::Message&, const FIX::SessionID&) override {}         // outgoing Logon/Heartbeat...
    
    void fromAdmin(const FIX::Message& message, const FIX::SessionID& sid) override { // incoming Logon/Heartbeat...
        // Called for every incoming admin message. We only check Logon.
        FIX::MsgType msgType;
        message.getHeader().getField(msgType);
        if (msgType.getValue() != FIX::MsgType_Logon) return;

        const FIX::Dictionary& d = settings_.get(sid);
        std::string user = message.isSetField(FIX::FIELD::Username)
            ? message.getField(FIX::FIELD::Username) : "";
        std::string pass = message.isSetField(FIX::FIELD::Password)
            ? message.getField(FIX::FIELD::Password) : "";
        if (user != d.getString("ExpectedUsername") ||
            pass != d.getString("ExpectedPassword")) {
            std::cout << "Rejecting logon from " << sid << " (bad credentials)\n";
            throw FIX::RejectLogon("Invalid username or password");
        }
    }

    void toApp(FIX::Message&, const FIX::SessionID&) override {}           // outgoing business message
    // Incoming business message. crack() reads tag 35 and calls the matching
    // onMessage() overload. If we have NO overload for that type (for example
    // a cancel, 35=F), crack() throws UnsupportedMessageType and QuickFIX
    // sends a BusinessMessageReject (35=j, 380=3) back automatically.
    void fromApp(const FIX::Message& m, const FIX::SessionID& s) override { crack(m, s); }

    // ------------------------------------------------------------------
    // 35=D NewOrderSingle arrives here
    // ------------------------------------------------------------------
    void onMessage(const FIX44::NewOrderSingle& o, const FIX::SessionID& s) override {
        // Read the fields we need. get() throws FieldNotFound if a tag is missing.
        FIX::ClOrdID clOrdID;   // 11  client's order id
        FIX::Symbol symbol;     // 55
        FIX::Side side;         // 54
        FIX::OrderQty qty;      // 38
        FIX::Price price(0);    // 44  optional (a market order has none)
        o.get(clOrdID);
        o.get(symbol);
        o.get(side);
        o.get(qty);
        if (o.isSetField(FIX::FIELD::Price)) o.get(price);

        // NOTE: toString() contains the invisible SOH separators, so this line
        // looks "glued together" on screen. The raw log shows the same message.
        std::cout << "Order received: " << o.toString() << "\n";

        // Increment ONCE and reuse the number. (Using ++id_ twice in one
        // expression gave OrderID and ExecID different values before.)
        int n = ++id_;

        // Build the ExecutionReport. The constructor takes the fields that
        // FIX 4.4 requires on a report; everything else is added with set().
        FIX44::ExecutionReport er(
            FIX::OrderID(std::to_string(n)),                // 37 exchange's order id
            FIX::ExecID(std::to_string(n)),                 // 17 id of this execution
            FIX::ExecType(FIX::ExecType_TRADE),             // 150 what just happened: a fill
            FIX::OrdStatus(FIX::OrdStatus_FILLED),          // 39 state of the order: filled
            side,                                           // 54
            FIX::LeavesQty(0),                              // 151 quantity still open
            FIX::CumQty(qty),                               // 14 total filled
            FIX::AvgPx(price));                             // 6 average price (0 for a market order: a simulator flaw)
        er.set(clOrdID);                    // 11 echo the client's id so it can match the fill
        er.set(symbol);                     // 55
        er.set(qty);                        // 38
        er.set(FIX::LastQty(qty));          // 32 quantity of this fill
        er.set(FIX::LastPx(price));         // 31 price of this fill

        // The session id 's' tells QuickFIX which session to send on.
        // It adds 34/49/52/56, the length and the checksum, stores the message,
        // and writes it to the socket.
        FIX::Session::sendToTarget(er, s);
    }
};

int main(int argc, char** argv) {
    try {
        // 1. Read acceptor.cfg (port, CompIDs, dictionary, store/log folders).
        FIX::SessionSettings settings(argc > 1 ? argv[1] : "acceptor.cfg");
        //ExecApp app;
        ExecApp app(settings);

        // 2. Store keeps sequence numbers + sent messages (for resend after a gap).
        FIX::FileStoreFactory store(settings);
        // 3. Log writes the raw messages and the session events.
        FIX::FileLogFactory log(settings);

        // 4. The server engine: (application, storeFactory, settings, logFactory).
        FIX::SocketAcceptor acceptor(app, store, settings, log);

        // 5. start() returns immediately; QuickFIX's threads accept connections.
        acceptor.start();
        std::cout << "Acceptor running. Press Enter to quit.\n";
        std::cin.get();             // keeps main() alive until you press Enter

        // 6. Clean shutdown: sends Logout and closes sockets.
        acceptor.stop();
    }
    catch (std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}