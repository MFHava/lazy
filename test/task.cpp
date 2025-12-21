
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

static_assert(!std::is_copy_constructible_v<decltype(std::declval<lazy::generator<int>>().begin())>);


auto flipflop() -> lazy::generator<int> {
	for(int i = 0; i < 8; ++i) {
		co_yield i % 2;
	}
	std::printf("\n");
}
TEST_CASE("generator flipflop", "[generator]") {
	auto t = []() -> lazy::task<void> {
		auto gen{flipflop()};

		for(auto beg = gen.begin(); co_await(beg != gen.end()); ++beg) {
			printf("%d ", *beg);
		}
	}();

	t.wait();
}
#if 1
auto iota() -> lazy::generator<int> {
	co_yield lazy::ranges::elements_of(flipflop());

	for(int i = 0; i < 10; ++i) {
		co_yield i;
	}
	std::printf("\n");
}

auto fibonacci() -> lazy::generator<int> {
	co_yield lazy::ranges::elements_of{iota()};

	auto a = 0, b = 1;
	for (;;) {
		co_yield std::exchange(a, std::exchange(b, a + b));
	}
	std::printf("\n");
}

TEST_CASE("generator fib", "[generator]") {
	auto t = []() -> lazy::task<void> {
		auto gen{fibonacci()};
		for(auto beg = gen.begin(); co_await(beg != gen.end()); ++beg) {
			auto && i{*beg};

			co_await []() -> lazy::task<void> {
				std::printf("nested task\n");
				co_return;
			}(); 

			if(i > 1000) break;
			std::printf("%d ", i);
		}
	}();
	t.wait();

	//auto it{fib.begin()};
	//*it;


#if 0
	auto it{fib.begin()};
	REQUIRE(*it == 0);
	REQUIRE(*it == 1);
	REQUIRE(*it == 1);
	REQUIRE(*it == 2);
	REQUIRE(*it == 3);
	REQUIRE(*it == 5);
	REQUIRE(*it == 8);
	REQUIRE(*it == 13);
	REQUIRE(*it == 21);
	REQUIRE(*it == 34);
#endif
}
#endif

