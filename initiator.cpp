#include "quickfix/Application.h"
#include "quickfix/MessageCracker.h"
#include "quickfix/FileStore.h"
#include "quickfix/FileLog.h"
#include "quickfix/SocketInitiator.h"
#include "quickfix/SessionSettings.h"
#include "quickfix/fix44/NewOrderSingle.h"
#include "quickfix/fix44/ExecutionReport.h"
#include <iostream>
#include <algorithm>

static std::string pretty(std::string s) {
    std::replace(s.begin(), s.end(), '\x01', '|');
    return s;
}

class ClientApp : public FIX::Application, public FIX::MessageCracker {
public:
    void onCreate(const FIX::SessionID&) override {}
    void onLogout(const FIX::SessionID& s) override { std::cout << "Logout: " << s << "\n"; }
    void toAdmin(FIX::Message&, const FIX::SessionID&) override {}
    void fromAdmin(const FIX::Message&, const FIX::SessionID&) override {}
    void toApp(FIX::Message&, const FIX::SessionID&) override {}
    void fromApp(const FIX::Message& m, const FIX::SessionID& s) override { crack(m, s); }

    void onLogon(const FIX::SessionID& s) override {
        std::cout << "Logon: " << s << "\n";
        FIX44::NewOrderSingle order(
            FIX::ClOrdID("ORD1"),
            FIX::Side(FIX::Side_BUY),
            FIX::TransactTime(),
            FIX::OrdType(FIX::OrdType_LIMIT));
        order.set(FIX::Symbol("AAPL"));
        order.set(FIX::OrderQty(100));
        order.set(FIX::Price(150.50));
        order.set(FIX::TimeInForce(FIX::TimeInForce_DAY));
        FIX::Session::sendToTarget(order, s);
        std::cout << "Sent: " << pretty(order.toString()) << "\n";
    }

    void onMessage(const FIX44::ExecutionReport& er, const FIX::SessionID&) override {
        std::cout << "ExecutionReport: " << pretty(er.toString()) << "\n";
    }
};

int main(int argc, char** argv) {
    try {
        FIX::SessionSettings settings(argc > 1 ? argv[1] : "initiator.cfg");
        ClientApp app;
        FIX::FileStoreFactory store(settings);
        FIX::FileLogFactory log(settings);
        FIX::SocketInitiator initiator(app, store, settings, log);
        initiator.start();
        std::cout << "Initiator running. Press Enter to quit.\n";
        std::cin.get();
        initiator.stop();
    } catch (std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}