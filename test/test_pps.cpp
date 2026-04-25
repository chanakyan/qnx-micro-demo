// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Rajeshkumar Venugopal / Third Buyer Advisory

#include <catch2/catch_test_macros.hpp>

import std;
import qnx.pps;

using namespace qnx;
using namespace qnx::pps;

TEST_CASE("create object returns valid id", "[pps]") {
    Pps pps;
    auto id = pps.create_object("/pps/system/battery");
    REQUIRE(id.has_value());
    REQUIRE(id->value > 0);
}

TEST_CASE("duplicate object creation fails", "[pps]") {
    Pps pps;
    (void)pps.create_object("/pps/system/battery");
    auto dup = pps.create_object("/pps/system/battery");
    REQUIRE(!dup.has_value());
    REQUIRE(dup.error() == KernelError::already_exists);
}

TEST_CASE("publish and read attribute", "[pps]") {
    Pps pps;
    (void)pps.publish("/pps/system/battery", "level", "87");
    auto val = pps.read("/pps/system/battery", "level");
    REQUIRE(val.has_value());
    REQUIRE(*val == "87");
}

TEST_CASE("publish auto-creates object", "[pps]") {
    Pps pps;
    REQUIRE(pps.object_count() == 0);
    (void)pps.publish("/pps/sensor/temp", "celsius", "22.5");
    REQUIRE(pps.object_count() == 1);
}

TEST_CASE("publish modifies existing attribute", "[pps]") {
    Pps pps;
    (void)pps.publish("/pps/system/battery", "level", "87");
    (void)pps.publish("/pps/system/battery", "level", "42");
    auto val = pps.read("/pps/system/battery", "level");
    REQUIRE(*val == "42");
}

TEST_CASE("read missing attribute returns not_found", "[pps]") {
    Pps pps;
    (void)pps.create_object("/pps/test");
    auto val = pps.read("/pps/test", "missing");
    REQUIRE(!val.has_value());
    REQUIRE(val.error() == KernelError::not_found);
}

TEST_CASE("read missing object returns not_found", "[pps]") {
    Pps pps;
    auto val = pps.read("/pps/nonexistent", "key");
    REQUIRE(!val.has_value());
}

TEST_CASE("subscriber gets notification on publish", "[pps]") {
    Pps pps;
    int notify_count = 0;
    std::string last_key;
    std::string last_value;

    (void)pps.subscribe("/pps/system/", ProcessId{1},
        [&](const Notification& n) {
            notify_count++;
            last_key = n.key;
            last_value = n.value;
        });

    (void)pps.publish("/pps/system/battery", "level", "87");
    REQUIRE(notify_count == 1);
    REQUIRE(last_key == "level");
    REQUIRE(last_value == "87");
}

TEST_CASE("subscriber gets notification on modify", "[pps]") {
    Pps pps;
    int notify_count = 0;

    (void)pps.subscribe("/pps/test", ProcessId{1},
        [&](const Notification&) { notify_count++; });

    (void)pps.publish("/pps/test", "key", "v1");
    (void)pps.publish("/pps/test", "key", "v2");
    REQUIRE(notify_count == 2);
}

TEST_CASE("subscriber does not get unrelated notifications", "[pps]") {
    Pps pps;
    int notify_count = 0;

    (void)pps.subscribe("/pps/system/", ProcessId{1},
        [&](const Notification&) { notify_count++; });

    (void)pps.publish("/pps/other/thing", "key", "val");
    REQUIRE(notify_count == 0);
}

TEST_CASE("unsubscribe stops notifications", "[pps]") {
    Pps pps;
    int notify_count = 0;

    auto sub = pps.subscribe("/pps/test", ProcessId{1},
        [&](const Notification&) { notify_count++; });

    (void)pps.publish("/pps/test", "key", "v1");
    REQUIRE(notify_count == 1);

    (void)pps.unsubscribe(*sub);
    (void)pps.publish("/pps/test", "key", "v2");
    REQUIRE(notify_count == 1);  // no new notification
}

TEST_CASE("delete object notifies subscribers", "[pps]") {
    Pps pps;
    int delete_count = 0;

    (void)pps.subscribe("/pps/test", ProcessId{1},
        [&](const Notification& n) {
            if (n.kind == Notification::Kind::deleted) delete_count++;
        });

    (void)pps.publish("/pps/test", "a", "1");
    (void)pps.publish("/pps/test", "b", "2");
    (void)pps.delete_object("/pps/test");
    REQUIRE(delete_count == 2);  // one per attribute
}

TEST_CASE("list objects under prefix", "[pps]") {
    Pps pps;
    (void)pps.publish("/pps/system/battery", "level", "87");
    (void)pps.publish("/pps/system/cpu", "load", "12");
    (void)pps.publish("/pps/app/music", "state", "playing");

    auto system = pps.list("/pps/system/");
    REQUIRE(system.size() == 2);

    auto all = pps.list("/pps/");
    REQUIRE(all.size() == 3);
}

TEST_CASE("read_object returns all attributes", "[pps]") {
    Pps pps;
    (void)pps.publish("/pps/hw/display", "brightness", "80");
    (void)pps.publish("/pps/hw/display", "resolution", "1920x1080");

    auto attrs = pps.read_object("/pps/hw/display");
    REQUIRE(attrs.has_value());
    REQUIRE(attrs->size() == 2);
}