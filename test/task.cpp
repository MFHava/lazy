
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

auto yolo() -> lazy::generator<char> {
	co_yield 'y';
	co_yield 'o';
	co_yield 'l';
	co_yield 'o';
}


auto flipflop() -> lazy::generator<int> {
	printf("flipflop\n");
	for(int i = 0; i < 8; ++i) {
		co_yield lazy::progress;
		co_yield i % 2;
		co_yield lazy::progress;
	}
}

auto iota() -> lazy::generator<int> {
	co_yield lazy::ranges::elements_of(flipflop());
	printf("iota\n");
	for(int i = 0; i < 10; ++i) {
		co_yield i;
	}
}

auto fibonacci() -> lazy::generator<int> {
	co_yield lazy::ranges::elements_of{iota()};
	printf("fibonacci\n");
	auto a = 0, b = 1;
	for (;;) {
		co_yield std::exchange(a, std::exchange(b, a + b));
	}
}

TEST_CASE("generator fib", "[generator]") {
	auto t = []() -> lazy::task<void> {
		auto gen{fibonacci()};
		for(auto beg = co_await gen.begin(); beg != gen.end(); co_await ++beg) {
			auto && i{*beg};

			if(i > 1000) break;
			std::printf("%d ", i);

			co_await []() -> lazy::task<void> {
				std::printf("nested task\n");
				co_return;
			}(); 

			auto g{yolo()};
			for(auto b = co_await g.begin(); b != g.end(); co_await ++b) std::printf("%c", *b);
			std::printf("\n");
		}
	}();


#if 0
	using namespace std::chrono_literals;
	while(not t.wait_for(0ms)) printf(" ===== ");
#else
	t.wait();
#endif
}

