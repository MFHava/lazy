
//          Copyright Michael Florian Hava.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file ../LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#include <array>
#include <memory_resource>

#include <catch2/catch_test_macros.hpp>
#include <lazy/lazy.hpp>

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

	template<typename T>
	struct myallocator final {
		int val;
	
		using value_type = T;
		using size_type = std::size_t;
		using difference_type = std::ptrdiff_t;


		template<typename U>
		myallocator(myallocator<U> other) : val{other.val} { printf("myallocator::rebind\n"); }

		myallocator(int val) noexcept : val{val} { printf("myallocator(int)\n"); }

		myallocator(const myallocator & other) : val{other.val} { printf("myallocator(const myallocator &)\n"); }
		myallocator(myallocator && other) noexcept : val{std::exchange(other.val, -2)} { printf("myallocator(const myallocator &)\n"); }
		auto operator=(const myallocator & other) noexcept -> myallocator & { printf("myallocator::operator=(const myallocator &)\n"); val = other.val; return *this; }
		auto operator=(myallocator && other) -> myallocator & { printf("myallocator::operator=(myallocator &&)\n"); val = std::exchange(other.val, -1); return *this; }
		~myallocator() noexcept { printf("~myallocator()\n"); }

		auto allocate(std::size_t size) -> T * {
			printf("myallocator::allocate: %d\n", val);
			return (T*)std::malloc(size);
		}
		void deallocate(T * ptr, std::size_t) {
			printf("myallocator::deallocate: %d\n", val);
			std::free(ptr);
		}
	};



TEST_CASE("stateless_allocator", "[lazy]") {
	myallocator<bool> a{10};

	std::array<std::byte, 1024> buffer{}; // enough to fit in all nodes
	std::pmr::monotonic_buffer_resource mbr{buffer.data(), buffer.size()};
	std::pmr::polymorphic_allocator<int> pa{&mbr};


	std::allocator<double> d;


	auto t{[](std::allocator_arg_t, auto...) -> lazy::task<int> {
		co_return 1;
	}(std::allocator_arg, pa)};

	REQUIRE(t.get() == 1);
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

TEST_CASE("nesting_started_before", "[lazy]") {
	using namespace std::chrono_literals;

	auto outer{[]() -> lazy::task<int> {
		auto middle{[]() -> lazy::task<int> {
			auto inner{[]() -> lazy::task<int> {
				auto gen{[]() -> lazy::generator<int> {
					co_yield co_await []() -> lazy::task<int> { co_return 0; }();
					co_await lazy::resumption;
					co_yield -1;
					co_await lazy::resumption;
					co_yield 0;
					co_await lazy::resumption;
					co_yield 1;
					co_await lazy::resumption;
					co_yield 0;
					co_await lazy::resumption;
					co_yield 1;
					co_await lazy::resumption;
				}()};

				int sum{0};
				for(auto it{co_await gen.begin()}; it != gen.end(); co_await ++it) {
					printf("%d\n", *it);
					co_yield lazy::progress;
					sum += *it;
				}
				co_yield lazy::progress;
				co_return sum;
			}()};

			inner.wait_for(0ms);
			co_return co_await std::move(inner) * 2;
		}()};
		middle.wait_for(0ms);
		co_return co_await std::move(middle) * 5;
	}()};

	REQUIRE(outer.get() == 10);
}

//TODO: timed waiting, etc.
static_assert(!std::is_copy_constructible_v<decltype(std::declval<lazy::generator<int>>().begin())>);

auto yolo() -> lazy::generator<char> {
	co_yield co_await []() -> lazy::task<char> { co_return 'x'; }() + 1;
	auto gen = []() -> lazy::generator<char> {
		co_yield 'n';
		co_yield co_await []() -> lazy::task<char> { co_return 'k'; }();
	}();
	for(auto it = co_await gen.begin(); it != gen.end(); co_await ++it) co_yield *it + 1;
	co_yield 'o';
}


auto flipflop() -> lazy::generator<int> {
	printf("flipflop\n");
	for(int i = 0; i < 8; ++i) {
		co_await lazy::resumption;
		co_yield i % 2;
		co_await lazy::resumption;
	}
}

auto iota() -> lazy::generator<int> {
	co_await flipflop();
	printf("iota\n");
	for(int i = 0; i < 10; ++i) {
		co_yield i;
	}
}

auto fibonacci() -> lazy::generator<int> {
	co_await iota();
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

		std::printf("%s\n", (co_await []() -> lazy::task<std::string> { co_return "=========== DONE ==========="; }()).c_str());
	}();


#if 0
	using namespace std::chrono_literals;
	while(not t.wait_for(0ms)) printf(" ===== ");
#else
	t.wait();
#endif
}
