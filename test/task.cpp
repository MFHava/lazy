
//          Copyright Michael Florian Hava.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file ../LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#include <catch2/catch_test_macros.hpp>
#include <lazy/task.hpp>

TEST_CASE("trivial", "[lazy]") {
	auto t{[]() -> lazy::task<int> { co_return 1; }()};
	REQUIRE(not t.valueless());

	t.wait();
	REQUIRE(not t.valueless());

	REQUIRE(t.get() == 1);
	REQUIRE(not t.valueless());
}

TEST_CASE("throwing_makes_valueless", "[lazy]") {
	auto t{[]() -> lazy::task<void> {
		throw 0;
		co_return;
	}()};
	REQUIRE(not t.valueless());

	try { t.wait(); }
	catch(...) {}
	REQUIRE(t.valueless());
}

TEST_CASE("nesting", "[lazy]") {
	auto t{[]() -> lazy::task<double> {
		auto v0 = co_await []() -> lazy::task<int> { co_return 10; }();
		REQUIRE(v0 == 10);

		auto v1 = co_await []() -> lazy::task<float> {
			co_return co_await []() -> lazy::task<int> { co_return 2; }();
		}();
		REQUIRE(v1 == 2.f);

		co_return v0 / v1;
	}()};

	REQUIRE(t.get() == 5.0);
}

//TODO: timed waiting, etc.

