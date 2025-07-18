/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cinttypes>
#include <regex>
#include <set>
#include <string>

#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <netdutils/Stopwatch.h>

#define LOG_TAG "Netd"
#include <log/log.h>

#include "ConnmarkFlags.h"
#include "Controllers.h"
#include "IdletimerController.h"
#include "NetworkController.h"
#include "RouteController.h"
#include "XfrmController.h"
#include "oem_iptables_hook.h"

namespace android {
namespace net {

using android::base::Join;
using android::base::StringAppendF;
using android::base::StringPrintf;
using android::netdutils::Stopwatch;

auto Controllers::execIptablesRestore  = ::execIptablesRestore;
auto Controllers::execIptablesRestoreWithOutput = ::execIptablesRestoreWithOutput;

netdutils::Log gLog("netd");
netdutils::Log gUnsolicitedLog("netdUnsolicited");

namespace {

static constexpr char CONNMARK_MANGLE_INPUT[] = "connmark_mangle_INPUT";
static constexpr char CONNMARK_MANGLE_OUTPUT[] = "connmark_mangle_OUTPUT";

/**
 * List of module chains to be created, along with explicit ordering. ORDERING
 * IS CRITICAL, AND SHOULD BE TRIPLE-CHECKED WITH EACH CHANGE.
 */
static const std::vector<const char*> FILTER_INPUT = {
        // Bandwidth should always be early in input chain, to make sure we
        // correctly count incoming traffic against data plan.
        OEM_IPTABLES_FILTER_INPUT,
        BandwidthController::LOCAL_INPUT,
        FirewallController::LOCAL_INPUT,
};

static const std::vector<const char*> FILTER_FORWARD = {
        OEM_IPTABLES_FILTER_FORWARD,
        FirewallController::LOCAL_FORWARD,
        BandwidthController::LOCAL_FORWARD,
        TetherController::LOCAL_FORWARD,
};

static const std::vector<const char*> FILTER_OUTPUT = {
        OEM_IPTABLES_FILTER_OUTPUT,
        FirewallController::LOCAL_OUTPUT,
        StrictController::LOCAL_OUTPUT,
        BandwidthController::LOCAL_OUTPUT,
};

static const std::vector<const char*> RAW_PREROUTING = {
        IdletimerController::LOCAL_RAW_PREROUTING,
        BandwidthController::LOCAL_RAW_PREROUTING,
        TetherController::LOCAL_RAW_PREROUTING,
};

static const std::vector<const char*> MANGLE_POSTROUTING = {
        OEM_IPTABLES_MANGLE_POSTROUTING,
        BandwidthController::LOCAL_MANGLE_POSTROUTING,
        IdletimerController::LOCAL_MANGLE_POSTROUTING,
};

static const std::vector<const char*> MANGLE_INPUT = {
        CONNMARK_MANGLE_INPUT,
        WakeupController::LOCAL_MANGLE_INPUT,
        RouteController::LOCAL_MANGLE_INPUT,
};

static const std::vector<const char*> MANGLE_FORWARD = {
        TetherController::LOCAL_MANGLE_FORWARD,
};

static const std::vector<const char*> MANGLE_OUTPUT = {
        CONNMARK_MANGLE_OUTPUT,
};

static const std::vector<const char*> NAT_PREROUTING = {
        OEM_IPTABLES_NAT_PREROUTING,
};

static const std::vector<const char*> NAT_POSTROUTING = {
        TetherController::LOCAL_NAT_POSTROUTING,
};

// Commands to create child chains and to match created chains in iptables -S output. Keep in sync.
static const char* CHILD_CHAIN_TEMPLATE = "-A %s -j %s\n";
static const std::regex CHILD_CHAIN_REGEX("^-A ([^ ]+) -j ([^ ]+)$",
                                          std::regex_constants::extended);

}  // namespace

/* static */
std::set<std::string> Controllers::findExistingChildChains(const IptablesTarget target,
                                                           const char* table,
                                                           const char* parentChain) {
    if (target == V4V6) {
        ALOGE("findExistingChildChains only supports one protocol at a time");
        abort();
    }

    std::set<std::string> existing;

    // List the current contents of parentChain.
    //
    // TODO: there is no guarantee that nothing else modifies the chain in the few milliseconds
    // between when we list the existing rules and when we delete them. However:
    // - Since this code is only run on startup, nothing else in netd will be running.
    // - While vendor code is known to add its own rules to chains created by netd, it should never
    //   be modifying the rules in childChains or the rules that hook said chains into their parent
    //   chains.
    std::string command = StringPrintf("*%s\n-S %s\nCOMMIT\n", table, parentChain);
    std::string output;
    if (Controllers::execIptablesRestoreWithOutput(target, command, &output) == -1) {
        ALOGE("Error listing chain %s in table %s", parentChain, table);
        return existing;
    }

    // The only rules added by createChildChains are of the simple form "-A <parent> -j <child>".
    // Find those rules and add each one's child chain to existing.
    std::smatch matches;
    std::stringstream stream(output);
    std::string rule;
    while (std::getline(stream, rule, '\n')) {
        if (std::regex_search(rule, matches, CHILD_CHAIN_REGEX) && matches[1] == parentChain) {
            existing.insert(matches[2]);
        }
    }

    return existing;
}

/* static */
void Controllers::createChildChains(IptablesTarget target, const char* table,
                                    const char* parentChain,
                                    const std::vector<const char*>& childChains,
                                    bool exclusive) {
    std::string command = StringPrintf("*%s\n", table);

    // We cannot just clear all the chains we create because vendor code modifies filter OUTPUT and
    // mangle POSTROUTING directly. So:
    //
    // - If we're the exclusive owner of this chain, simply clear it entirely.
    // - If not, then list the chain's current contents to ensure that if we restart after a crash,
    //   we leave the existing rules alone in the positions they currently occupy. This is faster
    //   than blindly deleting our rules and recreating them, because deleting a rule that doesn't
    //   exists causes iptables-restore to quit, which takes ~30ms per delete. It's also more
    //   correct, because if we delete rules and re-add them, they'll be in the wrong position with
    //   regards to the vendor rules.
    //
    // TODO: Make all chains exclusive once vendor code uses the oem_* rules.
    std::set<std::string> existingChildChains;
    if (exclusive) {
        // Just running ":chain -" flushes user-defined chains, but not built-in chains like INPUT.
        // Since at this point we don't know if parentChain is a built-in chain, do both.
        StringAppendF(&command, ":%s -\n", parentChain);
        StringAppendF(&command, "-F %s\n", parentChain);
    } else {
        existingChildChains = findExistingChildChains(target, table, parentChain);
    }

    for (const auto& childChain : childChains) {
        // Always clear the child chain.
        StringAppendF(&command, ":%s -\n", childChain);
        // But only add it to the parent chain if it's not already there.
        if (existingChildChains.find(childChain) == existingChildChains.end()) {
            StringAppendF(&command, CHILD_CHAIN_TEMPLATE, parentChain, childChain);
        }
    }
    command += "COMMIT\n";
    execIptablesRestore(target, command);
}

Controllers::Controllers()
    : wakeupCtrl(
              [this](const WakeupController::ReportArgs& args) {
                  const auto listener = eventReporter.getNetdEventListener();
                  if (listener == nullptr) {
                      gLog.error("getNetdEventListener() returned nullptr. dropping wakeup event");
                      return;
                  }
                  String16 prefix = String16(args.prefix.c_str());
                  String16 srcIp = String16(args.srcIp.c_str());
                  String16 dstIp = String16(args.dstIp.c_str());
                  listener->onWakeupEvent(prefix, args.uid, args.ethertype, args.ipNextHeader,
                                          args.dstHw, srcIp, dstIp, args.srcPort, args.dstPort,
                                          args.timestampNs);
              },
              &iptablesRestoreCtrl) {
    InterfaceController::initializeAll();
}

void Controllers::initChildChains() {
    /*
     * This is the only time we touch top-level chains in iptables; controllers
     * should only mutate rules inside of their children chains, as created by
     * the constants above.
     *
     * Modules should never ACCEPT packets (except in well-justified cases);
     * they should instead defer to any remaining modules using RETURN, or
     * otherwise DROP/REJECT.
     */

    // Create chains for child modules.
    createChildChains(V4V6, "filter", "INPUT", FILTER_INPUT, true);
    createChildChains(V4V6, "filter", "FORWARD", FILTER_FORWARD, true);
    createChildChains(V4V6, "raw", "PREROUTING", RAW_PREROUTING, true);
    createChildChains(V4V6, "mangle", "FORWARD", MANGLE_FORWARD, true);
    createChildChains(V4V6, "mangle", "INPUT", MANGLE_INPUT, true);
    createChildChains(V4V6, "mangle", "OUTPUT", MANGLE_OUTPUT, true);
    createChildChains(V4, "nat", "PREROUTING", NAT_PREROUTING, true);
    createChildChains(V4, "nat", "POSTROUTING", NAT_POSTROUTING, true);

    createChildChains(V4, "filter", "OUTPUT", FILTER_OUTPUT, false);
    createChildChains(V6, "filter", "OUTPUT", FILTER_OUTPUT, false);
    createChildChains(V4, "mangle", "POSTROUTING", MANGLE_POSTROUTING, false);
    createChildChains(V6, "mangle", "POSTROUTING", MANGLE_POSTROUTING, false);
}

static void setupConnmarkIptablesHooks() {
    // Rules to store parts of the fwmark (namely: netId, explicitlySelected, protectedFromVpn,
    // permission) in connmark.
    // Only saves the mark if no mark has been set before.
    static_assert(std::string_view(CONNMARK_MANGLE_INPUT) == "connmark_mangle_INPUT");
    static_assert(std::string_view(CONNMARK_MANGLE_OUTPUT) == "connmark_mangle_OUTPUT");
    static_assert(CONNMARK_FWMARK_MASK == 0x000FFFFF);
    const std::string cmd(
            // CONNMARK:
            // --save-mark [--nfmask nfmask] [--ctmask ctmask]
            // Copy the packet mark (nfmark) to the connection mark (ctmark) using the given
            // masks. The new nfmark value is determined as follows:
            // ctmark = (ctmark & ~ctmask) ^ (nfmark & nfmask)
            // i.e. ctmask defines what bits to clear and nfmask what bits of the nfmark to
            // XOR into the ctmark. ctmask and nfmask default to 0xFFFFFFFF.
            "*mangle\n"
            "-A connmark_mangle_INPUT -m connmark --mark 0/0x000FFFFF "
            "-j CONNMARK --save-mark --ctmask 0x000FFFFF --nfmask 0x000FFFFF\n"
            "-A connmark_mangle_OUTPUT -m connmark --mark 0/0x000FFFFF "
            "-j CONNMARK --save-mark --ctmask 0x000FFFFF --nfmask 0x000FFFFF\n"
            "COMMIT\n");
    execIptablesRestore(V4V6, cmd);
}

void Controllers::initIptablesRules() {
    Stopwatch s;
    initChildChains();
    gLog.info("Creating child chains: %" PRId64 "us", s.getTimeAndResetUs());

    // Let each module setup their child chains
    setupOemIptablesHook();
    gLog.info("Setting up OEM hooks: %" PRId64 "us", s.getTimeAndResetUs());

    /* When enabled, DROPs all packets except those matching rules. */
    firewallCtrl.setupIptablesHooks();
    gLog.info("Setting up FirewallController hooks: %" PRId64 "us", s.getTimeAndResetUs());

    /* Does DROPs in FORWARD by default */
    tetherCtrl.setupIptablesHooks();
    gLog.info("Setting up TetherController hooks: %" PRId64 "us", s.getTimeAndResetUs());

    /*
     * Does REJECT in INPUT, OUTPUT. Does counting also.
     * No DROP/REJECT allowed later in netfilter-flow hook order.
     */
    bandwidthCtrl.setupIptablesHooks();
    gLog.info("Setting up BandwidthController hooks: %" PRId64 "us", s.getTimeAndResetUs());

    /*
     * Counts in nat: PREROUTING, POSTROUTING.
     * No DROP/REJECT allowed later in netfilter-flow hook order.
     */
    idletimerCtrl.setupIptablesHooks();
    gLog.info("Setting up IdletimerController hooks: %" PRId64 "us", s.getTimeAndResetUs());

    /*
     * Add rules for detecting IPv6/IPv4 TCP/UDP connections with TLS/DTLS header
     */
    strictCtrl.setupIptablesHooks();
    gLog.info("Setting up StrictController hooks: %" PRId64 "us", s.getTimeAndResetUs());

    /*
     * Add rules for storing netid in connmark.
     */
    setupConnmarkIptablesHooks();
    gLog.info("Setting up connmark hooks: %" PRId64 "us", s.getTimeAndResetUs());
}

void Controllers::init() {
    initIptablesRules();
    Stopwatch s;

    if (int ret = bandwidthCtrl.enableBandwidthControl()) {
        gLog.error("Failed to initialize BandwidthController (%s)", strerror(-ret));
        // A failure to init almost definitely means that iptables failed to load
        // our static ruleset, which then basically means network accounting will not work.
        // As such simply exit netd.  This may crash loop the system, but by failing
        // to bootup we will trigger rollback and thus this offers us protection against
        // a mainline update breaking things.
        exit(1);
    }
    gLog.info("Enabling bandwidth control: %" PRId64 "us", s.getTimeAndResetUs());

    if (int ret = RouteController::Init(NetworkController::LOCAL_NET_ID)) {
        gLog.error("Failed to initialize RouteController (%s)", strerror(-ret));
        exit(2);
    }
    gLog.info("Initializing RouteController: %" PRId64 "us", s.getTimeAndResetUs());

    netdutils::Status xStatus = XfrmController::Init();
    if (!isOk(xStatus)) {
        gLog.error("Failed to initialize XfrmController (%s)", netdutils::toString(xStatus).c_str());
        exit(3);
    };
    gLog.info("Initializing XfrmController: %" PRId64 "us", s.getTimeAndResetUs());
}

Controllers* gCtls = nullptr;

}  // namespace net
}  // namespace android
