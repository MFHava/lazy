
//          Copyright Michael Florian Hava.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file ../LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#include <catch2/catch_test_macros.hpp>
#include <lazy/task.hpp>

auto func() -> lazy::task<int> {
	co_return 1;
}


TEST_CASE("trivial", "[lazy]") {
	auto t{func()};
	REQUIRE(not t.valueless());
	t.wait();
	REQUIRE(not t.valueless());
	REQUIRE(t.get() == 1);
	REQUIRE(not t.valueless());
}

//TODO

