#include "quickfix/Application.h"
#include "quickfix/MessageCracker.h"
#include "quickfix/FileStore.h"
#include "quickfix/FileLog.h"
#include "quickfix/SocketAcceptor.h"
#include "quickfix/SessionSettings.h"
#include "quickfix/fix44/NewOrderSingle.h"
#include "quickfix/fix44/ExecutionReport.h"
#include <iostream>
#include <string>

class ExecApp : public FIX::Application, public FIX::MessageCracker {
    int id_ = 0;
public:
    void onCreate(const FIX::SessionID&) override {}
    void onLogon(const FIX::SessionID& s) override { std::cout << "Logon: " << s << "\n"; }
    void onLogout(const FIX::SessionID& s) override { std::cout << "Logout: " << s << "\n"; }
    void toAdmin(FIX::Message&, const FIX::SessionID&) override {}
    void fromAdmin(const FIX::Message&, const FIX::SessionID&) override {}
    void toApp(FIX::Message&, const FIX::SessionID&) override {}
    void fromApp(const FIX::Message& m, const FIX::SessionID& s) override { crack(m, s); }

    void onMessage(const FIX44::NewOrderSingle& o, const FIX::SessionID& s) override {
        FIX::ClOrdID clOrdID; FIX::Symbol symbol; FIX::Side side;
        FIX::OrderQty qty; FIX::Price price(0);
        o.get(clOrdID); o.get(symbol); o.get(side); o.get(qty);
        if (o.isSetField(FIX::FIELD::Price)) o.get(price);

        std::cout << "Order received: " << o.toString() << "\n";

        int n = ++id_;
        FIX44::ExecutionReport er(
            FIX::OrderID(std::to_string(n)),
            FIX::ExecID(std::to_string(n)),
            FIX::ExecType(FIX::ExecType_TRADE),
            FIX::OrdStatus(FIX::OrdStatus_FILLED),
            side, FIX::LeavesQty(0), FIX::CumQty(qty), FIX::AvgPx(price));
        er.set(clOrdID);
        er.set(symbol);
        er.set(qty);
        er.set(FIX::LastQty(qty));
        er.set(FIX::LastPx(price));
        FIX::Session::sendToTarget(er, s);
    }
};

int main(int argc, char** argv) {
    try {
        FIX::SessionSettings settings(argc > 1 ? argv[1] : "acceptor.cfg");
        ExecApp app;
        FIX::FileStoreFactory store(settings);
        FIX::FileLogFactory log(settings);
       	FIX::SocketAcceptor acceptor(app, store, settings, log);
        acceptor.start();
        std::cout << "Acceptor running. Press Enter to quit.\n";
        std::cin.get();
        acceptor.stop();
    } catch (std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}