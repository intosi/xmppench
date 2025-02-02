/*
 * Copyright (c) 2011 Tobias Markmann
 * Licensed under the GNU General Public License v3.
 * See gpl-3.0.txt for more information.
 */

#include <fstream>
#include <iostream>
#include <vector>

#include <boost/format.hpp>
#include <boost/program_options.hpp>
// #include <boost/thread.hpp>

// #include <Swiften/EventLoop/SimpleEventLoop.h>
#define private public // FIXME
#include <Swiften/EventLoop/BoostASIOEventLoop.h>
#undef private
#include <Swiften/JID/JID.h>
#include <Swiften/Network/BoostNetworkFactories.h>

#include "AccountDataProvider.h"
#include "LatencyWorkloadBenchmark.h"
#include "BenchmarkNetworkFactories.h"
// #include "BoostEventLoop.h"


#include <ctime>

#include <boost/date_time/microsec_time_clock.hpp>
#include <boost/date_time/posix_time/ptime.hpp>

#define XMPPENCH_VERSION_STRING "0.1"

using namespace Swift;
namespace po = boost::program_options;

class ContinuousAccountProivder : public AccountDataProvider {
public:
	ContinuousAccountProivder(const std::string& hostname, const std::string& rabbitprefix) : hostname(hostname), rabbitprefix(rabbitprefix), counter(0) { }

	virtual Account getAccount() {
		std::string jid  = boost::str( boost::format("%1%%2%@%3%") % rabbitprefix % counter % hostname );
		std::string password = boost::str( boost::format("%1%%2%") % rabbitprefix % counter);
		++counter;

		AccountDataProvider::Account acc;
		acc.jid = jid;
		acc.password = password;

		return acc;
	}

private:
	std::string hostname;
	std::string rabbitprefix;
	unsigned long counter;
};

void eventLoopRunner(BoostASIOEventLoop* eventLoop) {
	eventLoop->ioService_->run();
}

int main(int argc, char *argv[]) {
	std::string hostname;
	std::string ip;
	std::string bodyfile;
	std::string rabbitprefix;
	bool waitAtBeginning;
	size_t jobs = 1;
	std::string boshHost;
	std::string boshPath;
	int boshPort;
	bool boshHTTPS;

	LatencyWorkloadBenchmark::Options options;

	po::options_description desc("Allowed options");
	desc.add_options()
			("actives", po::value<int>(&options.noOfActiveSessions)->default_value(2000),		"number of active connections")
			("bodyfile", po::value<std::string>(&bodyfile),									"file to read the (unchecked!) body message from")
			("help",																			"produce help message")
			("hostname", po::value<std::string>(&hostname)->default_value("localhost"),		"hostname of benchmarked server")
			("idles", po::value<int>(&options.noOfIdleSessions)->default_value(8000),			"number of idle connections")
			("ip", po::value<std::string>(&ip),												"specify the IP to connect to; overrides DNS lookups; required with jobs > 1")
			("jobs", po::value<size_t>(&jobs)->default_value(1),									"number of threads to run ! EXPERIMENTAL !")
			("nocomp", po::value<bool>(&options.noCompression)->default_value(false),			"prevent use of stream compression")
			("notls", po::value<bool>(&options.noTLS)->default_value(false),					"prevent use of TLS")
			("plogins", po::value<size_t>(&options.parallelLogins)->default_value(2),			"number of parallel logins")
			("rabbitprefix", po::value<std::string>(&rabbitprefix),							"Prefix to use before number for accounts and passwords")
			("stanzas", po::value<int>(&options.stanzasPerConnection)->default_value(1000),	"stanzas to send per connection")
			("version",																		"print version number")
            ("waitatstart", po::value<bool>(&waitAtBeginning)->default_value(0),				"waits at the beginning on keyboard input")
			("wcstanzas", po::value<int>(&options.warmupStanzas)->default_value(0),			"warm up/cool down stanzas")
			("boshhost", po::value<std::string>(&boshHost), "specify to use BOSH for connection, via this host")
			("boshport", po::value<int>(&boshPort)->default_value(5280), "specify to change the BOSH port (when using BOSH)")
			("boshpath", po::value<std::string>(&boshPath)->default_value("http-bind/"), "specify to change the path used, if using BOSH")
			("boshhttps", "use HTTPS (not HTTP) for BOSH")
	;

	po::variables_map vm;
	po::parsed_options parsed = po::command_line_parser(argc, argv).options(desc).allow_unregistered().run();


	std::vector<std::string> unrecognizedOptions = collect_unrecognized(parsed.options, boost::program_options::include_positional);
	if (!unrecognizedOptions.empty()) {
		std::cout << "Unrecognized options!" << std::endl;
		std::cout << desc << std::endl;
		return 1;
	}

	po::store(parsed, vm);
	po::notify(vm);

	if (vm.count("help")) {
		std::cout << desc << "\n";
		return 1;
	}

	if (vm.count("version")) {
		std::cout << "xmppench - XMPP server benchmarking - Version " << XMPPENCH_VERSION_STRING << std::endl;
		return 1;
	}

	boshHTTPS = vm.count("boshhttps");

	if (waitAtBeginning) {
		char c;
		std::cin >> c;
	}

	if (jobs > 1 && ip.empty()) {
		std::cout << "Error: You have to specify the IP of the server (use --ip) if jobs is greater than 1." << std::endl;
		return 1;
	}
	if (jobs > 1) {
		std::cout << "Warning: Running multiple worker threads is an experimental feature." << std::endl;
	}

	if (options.parallelLogins < jobs) {
		options.parallelLogins = jobs;
	}

	if (!boshHost.empty()) {
		options.boshURL = URL(boshHTTPS ? "https" : "http", boshHost, boshPort, boshPath);
	}

	if (!bodyfile.empty()) {
		std::ifstream inputStream(bodyfile.c_str(), std::ios::in | std::ios::binary | std::ios::ate);

		std::ifstream::pos_type fSize = inputStream.tellg();
		inputStream.seekg(0, std::ios::beg);

		std::vector<char> bytes(fSize);
		inputStream.read(&bytes[0], fSize);

		options.bodymessage = std::string(&bytes[0], fSize);
	} else {
		options.bodymessage = "Hey. You forgot to specify a body message.";

	}

	std::vector<BoostASIOEventLoop*> eventLoops;
	std::vector<NetworkFactories*> networkFactories;

	for (size_t n = 0; n < jobs; ++n) {
		BoostASIOEventLoop* eventLoop = new BoostASIOEventLoop(std::make_shared<boost::asio::io_service>());
		NetworkFactories* factory;
		//if (jobs > 1) {
			factory = new BenchmarkNetworkFactories(eventLoop, ip);
		//} else {
		//	factory = new BoostNetworkFactories(eventLoop);
		//}
		eventLoops.push_back(eventLoop);
		networkFactories.push_back(factory);
	}

	ContinuousAccountProivder accountProvider(hostname, rabbitprefix);

	LatencyWorkloadBenchmark benchmark(networkFactories, &accountProvider, options);

	std::vector<std::thread> threadGroup;

	for (size_t n = 0; n < jobs; ++n) {
		auto service = eventLoops[n]->ioService_;
		threadGroup.emplace_back([service](){service->run();});
	}
	for (auto& thread : threadGroup) {
		thread.join();
	}
	for (size_t i = 0; i < jobs; ++i) {
		delete eventLoops[i];
		delete networkFactories[i];
	}
	return 0;
}
