/*
	This file is part of cpp-ethereum.

	cpp-ethereum is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	cpp-ethereum is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/
/** @file whisperTopic.cpp
 * @author Gav Wood <i@gavwood.com>
 * @date 2014
 */
#include <functional>

#include <boost/test/unit_test.hpp>

#include <libp2p/Host.h>
#include <libwhisper/WhisperPeer.h>
#include <libwhisper/WhisperHost.h>
using namespace std;
using namespace dev;
using namespace dev::p2p;
using namespace dev::shh;

struct P2PFixture
{
	P2PFixture() { dev::p2p::NodeIPEndpoint::test_allowLocal = true; }
	~P2PFixture() { dev::p2p::NodeIPEndpoint::test_allowLocal = false; }
};

BOOST_FIXTURE_TEST_SUITE(whisper, P2PFixture)

BOOST_AUTO_TEST_CASE(topic)
{
	cnote << "Testing Whisper...";
	auto oldLogVerbosity = g_logVerbosity;
	g_logVerbosity = 0;

	Host host1("Test", NetworkPreferences("127.0.0.1", 30303, false));
	host1.setIdealPeerCount(1);
	auto whost1 = host1.registerCapability(new WhisperHost());
	host1.start();

	bool host1Ready = false;
	unsigned result = 0;
	std::thread listener([&]()
	{
		setThreadName("other");
		
		/// Only interested in odd packets
		auto w = whost1->installWatch(BuildTopicMask("odd"));
		host1Ready = true;
		set<unsigned> received;
		for (int iterout = 0, last = 0; iterout < 200 && last < 81; ++iterout)
		{
			for (auto i: whost1->checkWatch(w))
			{
				Message msg = whost1->envelope(i).open(whost1->fullTopics(w));
				last = RLP(msg.payload()).toInt<unsigned>();
				if (received.count(last))
					continue;
				received.insert(last);
				cnote << "New message from:" << msg.from() << RLP(msg.payload()).toInt<unsigned>();
				result += last;
			}
			this_thread::sleep_for(chrono::milliseconds(50));
		}

	});

	Host host2("Test", NetworkPreferences("127.0.0.1", 30300, false));
	host1.setIdealPeerCount(1);
	auto whost2 = host2.registerCapability(new WhisperHost());
	host2.start();
	
	while (!host1.haveNetwork())
		this_thread::sleep_for(chrono::milliseconds(5));
	host2.addNode(host1.id(), NodeIPEndpoint(bi::address::from_string("127.0.0.1"), 30303, 30303));

	// wait for nodes to connect
	this_thread::sleep_for(chrono::milliseconds(1000));
	
	while (!host1Ready)
		this_thread::sleep_for(chrono::milliseconds(10));
	
	KeyPair us = KeyPair::create();
	for (int i = 0; i < 10; ++i)
	{
		whost2->post(us.sec(), RLPStream().append(i * i).out(), BuildTopic(i)(i % 2 ? "odd" : "even"));
		this_thread::sleep_for(chrono::milliseconds(250));
	}

	listener.join();
	g_logVerbosity = oldLogVerbosity;

	BOOST_REQUIRE_EQUAL(result, 1 + 9 + 25 + 49 + 81);
}

BOOST_AUTO_TEST_CASE(forwarding)
{
	cnote << "Testing Whisper forwarding...";
	auto oldLogVerbosity = g_logVerbosity;
	g_logVerbosity = 0;

	// Host must be configured not to share peers.
	Host host1("Listner", NetworkPreferences("127.0.0.1", 30303, false));
	host1.setIdealPeerCount(1);
	auto whost1 = host1.registerCapability(new WhisperHost());
	host1.start();
	while (!host1.haveNetwork())
		this_thread::sleep_for(chrono::milliseconds(2));

	unsigned result = 0;
	bool done = false;

	bool startedListener = false;
	std::thread listener([&]()
	{
		setThreadName("listener");

		startedListener = true;

		/// Only interested in odd packets
		auto w = whost1->installWatch(BuildTopicMask("test"));

		for (int i = 0; i < 200 && !result; ++i)
		{
			for (auto i: whost1->checkWatch(w))
			{
				Message msg = whost1->envelope(i).open(whost1->fullTopics(w));
				unsigned last = RLP(msg.payload()).toInt<unsigned>();
				cnote << "New message from:" << msg.from() << RLP(msg.payload()).toInt<unsigned>();
				result = last;
			}
			this_thread::sleep_for(chrono::milliseconds(50));
		}
	});


	// Host must be configured not to share peers.
	Host host2("Forwarder", NetworkPreferences("127.0.0.1", 30305, false));
	host2.setIdealPeerCount(1);
	auto whost2 = host2.registerCapability(new WhisperHost());
	host2.start();
	while (!host2.haveNetwork())
		this_thread::sleep_for(chrono::milliseconds(2));

	Public fwderid;
	bool startedForwarder = false;
	std::thread forwarder([&]()
	{
		setThreadName("forwarder");

		while (!startedListener)
			this_thread::sleep_for(chrono::milliseconds(50));

		this_thread::sleep_for(chrono::milliseconds(500));
		host2.addNode(host1.id(), NodeIPEndpoint(bi::address::from_string("127.0.0.1"), 30303, 30303));

		startedForwarder = true;

		/// Only interested in odd packets
		auto w = whost2->installWatch(BuildTopicMask("test"));

		while (!done)
		{
			for (auto i: whost2->checkWatch(w))
			{
				Message msg = whost2->envelope(i).open(whost2->fullTopics(w));
				cnote << "New message from:" << msg.from() << RLP(msg.payload()).toInt<unsigned>();
			}
			this_thread::sleep_for(chrono::milliseconds(50));
		}
	});

	while (!startedForwarder)
		this_thread::sleep_for(chrono::milliseconds(50));

	Host ph("Sender", NetworkPreferences("127.0.0.1", 30300, false));
	ph.setIdealPeerCount(1);
	shared_ptr<WhisperHost> wh = ph.registerCapability(new WhisperHost());
	ph.start();
	ph.addNode(host2.id(), NodeIPEndpoint(bi::address::from_string("127.0.0.1"), 30305, 30305));
	while (!ph.haveNetwork())
		this_thread::sleep_for(chrono::milliseconds(10));

	while (!ph.peerCount())
		this_thread::sleep_for(chrono::milliseconds(10));

	KeyPair us = KeyPair::create();
	wh->post(us.sec(), RLPStream().append(1).out(), BuildTopic("test"));
	this_thread::sleep_for(chrono::milliseconds(250));

	listener.join();
	done = true;
	forwarder.join();
	g_logVerbosity = oldLogVerbosity;

	BOOST_REQUIRE_EQUAL(result, 1);
}

BOOST_AUTO_TEST_CASE(asyncforwarding)
{
	cnote << "Testing Whisper async forwarding...";
	auto oldLogVerbosity = g_logVerbosity;
	g_logVerbosity = 2;

	unsigned result = 0;
	bool done = false;

	// Host must be configured not to share peers.
	Host host1("Forwarder", NetworkPreferences("127.0.0.1", 30305, false));
	host1.setIdealPeerCount(1);
	auto whost1 = host1.registerCapability(new WhisperHost());
	host1.start();
	while (!host1.haveNetwork())
		this_thread::sleep_for(chrono::milliseconds(2));

	bool startedForwarder = false;
	std::thread forwarder([&]()
	{
		setThreadName("forwarder");

		this_thread::sleep_for(chrono::milliseconds(500));

		startedForwarder = true;

		/// Only interested in odd packets
		auto w = whost1->installWatch(BuildTopicMask("test"));

		while (!done)
		{
			for (auto i: whost1->checkWatch(w))
			{
				Message msg = whost1->envelope(i).open(whost1->fullTopics(w));
				cnote << "New message from:" << msg.from() << RLP(msg.payload()).toInt<unsigned>();
			}
			this_thread::sleep_for(chrono::milliseconds(50));
		}
	});

	while (!startedForwarder)
		this_thread::sleep_for(chrono::milliseconds(2));

	{
		Host host2("Sender", NetworkPreferences("127.0.0.1", 30300, false));
		host2.setIdealPeerCount(1);
		shared_ptr<WhisperHost> whost2 = host2.registerCapability(new WhisperHost());
		host2.start();
		while (!host2.haveNetwork())
			this_thread::sleep_for(chrono::milliseconds(2));
		host2.addNode(host1.id(), NodeIPEndpoint(bi::address::from_string("127.0.0.1"), 30305, 30305));

		while (!host2.peerCount())
			this_thread::sleep_for(chrono::milliseconds(5));

		KeyPair us = KeyPair::create();
		whost2->post(us.sec(), RLPStream().append(1).out(), BuildTopic("test"));
		this_thread::sleep_for(chrono::milliseconds(250));
	}

	{
		Host ph("Listener", NetworkPreferences("127.0.0.1", 30300, false));
		ph.setIdealPeerCount(1);
		shared_ptr<WhisperHost> wh = ph.registerCapability(new WhisperHost());
		ph.start();
		while (!ph.haveNetwork())
			this_thread::sleep_for(chrono::milliseconds(2));
		ph.addNode(host1.id(), NodeIPEndpoint(bi::address::from_string("127.0.0.1"), 30305, 30305));

		/// Only interested in odd packets
		auto w = wh->installWatch(BuildTopicMask("test"));

		for (int i = 0; i < 200 && !result; ++i)
		{
			for (auto i: wh->checkWatch(w))
			{
				Message msg = wh->envelope(i).open(wh->fullTopics(w));
				unsigned last = RLP(msg.payload()).toInt<unsigned>();
				cnote << "New message from:" << msg.from() << RLP(msg.payload()).toInt<unsigned>();
				result = last;
			}
			this_thread::sleep_for(chrono::milliseconds(50));
		}
	}

	done = true;
	forwarder.join();
	g_logVerbosity = oldLogVerbosity;

	BOOST_REQUIRE_EQUAL(result, 1);
}

BOOST_AUTO_TEST_SUITE_END()
