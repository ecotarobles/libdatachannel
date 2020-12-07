/*
 * libdatachannel client example
 * Copyright (c) 2019-2020 Paul-Louis Ageneau
 * Copyright (c) 2019 Murat Dogan
 * Copyright (c) 2020 Will Munn
 * Copyright (c) 2020 Nico Chatzi
 * Copyright (c) 2020 Lara Mackey
 * Copyright (c) 2020 Erik Cota-Robles
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; If not, see <http://www.gnu.org/licenses/>.
 */

#include "rtc/rtc.hpp"

#include "parse_cl.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <future>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <thread>
#include <unordered_map>

using namespace rtc;
using namespace std;
using namespace std::chrono_literals;

using json = nlohmann::json;

template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }

unordered_map<string, shared_ptr<PeerConnection>> peerConnectionMap;
unordered_map<string, shared_ptr<DataChannel>> dataChannelMap;

string localToken;
bool echoDataChannelMessages = false;

shared_ptr<PeerConnection> createPeerConnection(const Configuration &config,
						weak_ptr<WebSocket> wws, string token);
void printReceived(bool echoed, string token, size_t length, string type);
string randomToken(size_t length);

int main(int argc, char **argv) try {
	auto params = std::make_unique<Cmdline>(argc, argv);

	rtc::InitLogger(LogLevel::Info);

	Configuration config;
	if (params->noStun()) {
		cout << "No STUN server is configured. Only local hosts and public IP addresses supported."
		     << endl;
	} else {
		string stunServer = "";
		if (params->stunServer().substr(0, 5).compare("stun:") != 0) {
			stunServer = "stun:";
		}
		stunServer += params->stunServer() + ":" + to_string(params->stunPort());
		cout << "Stun server is " << stunServer << endl;
		config.iceServers.emplace_back(stunServer);
	}
	if (params->proxyServer().compare("localhost") != 0) {
		string proxyServer = "";
		proxyServer = params->proxyServer() + ":" + to_string(params->proxyPort());
		cout << "Proxy server is " << proxyServer << endl;
		config.proxyServer.emplace(rtc::ProxyServer::Type::Http, params->proxyServer(),
			params->proxyPort());
	}

	localToken = randomToken(4);
	cout << "The local ID is: " << localToken << endl;
	echoDataChannelMessages = params->echoDataChannelMessages();
	cout << "Received data channel messages will be "
		<< (echoDataChannelMessages ? "echoed back to sender" : "printed to stdout") << endl;

	auto ws = make_shared<WebSocket>();

	std::promise<void> wsPromise;
	auto wsFuture = wsPromise.get_future();

	ws->onOpen([&wsPromise]() {
		cout << "WebSocket connected, signaling ready" << endl;
		wsPromise.set_value();
	});

	ws->onError([&wsPromise](string s) {
		cout << "WebSocket error" << endl;
		wsPromise.set_exception(std::make_exception_ptr(std::runtime_error(s)));
	});

	ws->onClosed([]() { cout << "WebSocket closed" << endl; });

	ws->onMessage([&](variant<binary, string> data) {
		if (!holds_alternative<string>(data))
			return;

		json message = json::parse(get<string>(data));

		auto it = message.find("token");
		if (it == message.end())
			return;
		string token = it->get<string>();

		it = message.find("type");
		if (it == message.end())
			return;
		string type = it->get<string>();

		shared_ptr<PeerConnection> pc;
		if (auto jt = peerConnectionMap.find(token); jt != peerConnectionMap.end()) {
			pc = jt->second;
		} else if (type == "offer") {
			cout << "Answering to " + token << endl;
			pc = createPeerConnection(config, ws, token);
		} else {
			return;
		}

		if (type == "offer" || type == "answer") {
			auto sdp = message["description"].get<string>();
			pc->setRemoteDescription(Description(sdp, type));
		} else if (type == "candidate") {
			auto sdp = message["candidate"].get<string>();
			auto mid = message["mid"].get<string>();
			pc->addRemoteCandidate(Candidate(sdp, mid));
		}
	});

	string wsPrefix = "";
	if (params->webSocketServer().substr(0, 5).compare("ws://") != 0) {
		wsPrefix = "ws://";
	}
	const string url = wsPrefix + params->webSocketServer() + ":" +
	                   to_string(params->webSocketPort()) + "/" + localToken;
	cout << "Url is " << url << endl;
	ws->open(url);

	cout << "Waiting for signaling to be connected..." << endl;
	wsFuture.get();

	while (true) {
		string token;
		cout << "Enter a remote ID to send an offer:" << endl;
		cin >> token;
		cin.ignore();
		if (token.empty())
			break;
		if (token == localToken)
			continue;

		cout << "Offering to " + token << endl;
		auto pc = createPeerConnection(config, ws, token);

		// We are the offerer, so create a data channel to initiate the process
		const string label = "test";
		cout << "Creating DataChannel with label \"" << label << "\"" << endl;
		auto dc = pc->createDataChannel(label);

		dc->onOpen([token, wdc = make_weak_ptr(dc)]() {
			cout << "DataChannel from peer with " << token << " open" << endl;
			if (auto dc = wdc.lock())
				dc->send("Hello from " + localToken);
		});

		dc->onClosed([token]() { cout << "DataChannel from peer with " << token << " closed" << endl; });

		dc->onMessage([token, wdc = make_weak_ptr(dc)](variant<binary, string> data) {
			int len = holds_alternative<string>(data) ?
					get<string>(data).length() : get<binary>(data).size();
			if (echoDataChannelMessages) {
				bool echoed = false;
				if (auto dc = wdc.lock()) {
					dc->send(data);
					echoed = true;
				}
				printReceived(echoed, token, len,
					(holds_alternative<string>(data) ?  "text" : "binary"));
			} else if (holds_alternative<string>(data)) {
				if (len < 80) {
					cout << "Message from peer with " << token << " received: " <<
						get<string>(data) << endl;
				} else {
					cout << "Message from peer with " << token << " received: " <<
						get<string>(data).substr(0,80) << "..." << endl;
				}
			} else {
				cout << "Binary message from peer with " << token << " received, size=" <<
					get<binary>(data).size() << endl;
			}
		});

		dataChannelMap.emplace(token, dc);
	}

	cout << "Cleaning up..." << endl;

	dataChannelMap.clear();
	peerConnectionMap.clear();
	return 0;

} catch (const std::exception &e) {
	std::cout << "Error: " << e.what() << std::endl;
	dataChannelMap.clear();
	peerConnectionMap.clear();
	return -1;
}

// Create and setup a PeerConnection
shared_ptr<PeerConnection> createPeerConnection(const Configuration &config,
						weak_ptr<WebSocket> wws, string token) {
	auto pc = make_shared<PeerConnection>(config);

	pc->onStateChange([](PeerConnection::State state) { cout << "State: " << state << endl; });

	pc->onGatheringStateChange(
	    [](PeerConnection::GatheringState state) { cout << "Gathering State: " << state << endl; });

	pc->onLocalDescription([wws, token](Description description) {
		json message = {
		    {"token", token}, {"type", description.typeString()}, {"description", string(description)}};

		if (auto ws = wws.lock())
			ws->send(message.dump());
	});

	pc->onLocalCandidate([wws, token](Candidate candidate) {
		json message = {{"token", token},
		                {"type", "candidate"},
		                {"candidate", string(candidate)},
		                {"mid", candidate.mid()}};

		if (auto ws = wws.lock())
			ws->send(message.dump());
	});

	pc->onDataChannel([token](shared_ptr<DataChannel> dc) {
		cout << "DataChannel from peer with " << token << " received with label \"" << dc->label() << "\""
		     << endl;

		dc->onClosed([token]() { cout << "DataChannel from peer with " << token << " closed" << endl; });

		dc->onMessage([token, wdc = make_weak_ptr(dc)](variant<binary, string> data) {
			int len = holds_alternative<string>(data) ?
					get<string>(data).length() : get<binary>(data).size();
			if (echoDataChannelMessages) {
				bool echoed = false;
				if (auto dc = wdc.lock()) {
					dc->send(data);
					echoed = true;
				}
				printReceived(echoed, token, len,
					(holds_alternative<string>(data) ?  "text" : "binary"));
			} else if (holds_alternative<string>(data)) {
				if (len < 80) {
					cout << "Message from peer with " << token << " received: " <<
						get<string>(data) << endl;
				} else {
					cout << "Message from peer with " << token << " received: " <<
						get<string>(data).substr(0,80) << "..." << endl;
				}
			} else {
				cout << "Binary message from peer with " << token << " received, size=" <<
					get<binary>(data).size() << endl;
			}
		});

		dc->send("Hello from peer with " + localToken);

		dataChannelMap.emplace(token, dc);
	});

	peerConnectionMap.emplace(token, pc);
	return pc;
};

// Helper function to print received pings
void printReceived(bool echoed, string token, size_t length, string type) {
	static long count = 0;
	static long freq = 100;
	if (!(++count%freq)) {
		cout << "Received " << count << " pings in total from peer with " << token <<
			", most recent of type " << type << " and " <<
			(echoed ? "" : "un") << "successfully echoed most recent ping " <<
			"of size " << length << " back to " << token << endl;
		if (count >= (freq * 10) && freq < 1000000) {
			freq *= 10;
		}
	}
}


// Helper function to generate a random ID
string randomToken(size_t length) {
	static const string characters(
	    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
	string token(length, '0');
	default_random_engine rng(random_device{}());
	uniform_int_distribution<int> dist(0, int(characters.size() - 1));
	generate(token.begin(), token.end(), [&]() { return characters.at(dist(rng)); });
	return token;
}
