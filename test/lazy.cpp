
//          Copyright Michael Florian Hava.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file ../LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#include <array>
#include <print>
#include <ranges>
#include <thread>
#include <iostream>
#include <memory_resource>

#include <catch2/catch_test_macros.hpp>
#include <lazy/lazy.hpp>

TEST_CASE("trivial", "[lazy]") {
	auto t{[]() -> lazy::root_task<int> { co_return 1; }()};
	REQUIRE(not t.valueless());

	REQUIRE(t.wait() == lazy::state::done);
	REQUIRE(not t.valueless());

	REQUIRE(t.has_result());
	REQUIRE(t.result() == 1);
	REQUIRE(not t.valueless());
}

TEST_CASE("throwing_not_makes_valueless", "[lazy]") {
	auto t{[]() -> lazy::root_task<void> {
		throw 0;
		co_return;
	}()};
	REQUIRE(not t.valueless());

	try { t.wait(); }
	catch(...) {}
	REQUIRE(not t.valueless());
	REQUIRE(not t.has_result());
	t.result();
}

	template<typename T>
	struct myallocator final {
		int val;
	
		using value_type = T;
		using size_type = std::size_t;
		using difference_type = std::ptrdiff_t;


		template<typename U>
		myallocator(myallocator<U> other) : val{other.val} { std::println("myallocator::rebind"); }

		myallocator(int val) noexcept : val{val} { std::println("myallocator(int)"); }

		myallocator(const myallocator & other) : val{other.val} { std::println("myallocator(const myallocator &)"); }
		myallocator(myallocator && other) noexcept : val{std::exchange(other.val, -2)} { std::println("myallocator(const myallocator &)"); }
		auto operator=(const myallocator & other) noexcept -> myallocator & { std::println("myallocator::operator=(const myallocator &)"); val = other.val; return *this; }
		auto operator=(myallocator && other) -> myallocator & { std::println("myallocator::operator=(myallocator &&)"); val = std::exchange(other.val, -1); return *this; }
		~myallocator() noexcept { std::println("~myallocator()"); }

		auto allocate(std::size_t size) -> T * {
			std::println("myallocator::allocate: {}", val);
			return (T*)std::malloc(size);
		}
		void deallocate(T * ptr, std::size_t) {
			std::println("myallocator::deallocate: {}", val);
			std::free(ptr);
		}
	};

TEST_CASE("stateless_allocator", "[lazy]") {
	myallocator<bool> a{10};

	std::array<std::byte, 1024> buffer{}; // enough to fit in all nodes
	std::pmr::monotonic_buffer_resource mbr{buffer.data(), buffer.size()};
	std::pmr::polymorphic_allocator<int> pa{&mbr};

	auto t{[](std::allocator_arg_t, auto...) -> lazy::root_task<int> {
		co_return 1;
	}(std::allocator_arg, pa)};

	REQUIRE(t.wait() == lazy::state::done);
	REQUIRE(t.has_result());
	REQUIRE(t.result() == 1);
}

TEST_CASE("nesting", "[lazy]") {
	auto t{[]() -> lazy::root_task<double> {
		auto v0 = co_await []() -> lazy::task<int> { co_return 10; }();
		REQUIRE(v0 == 10);

		auto v1 = co_await []() -> lazy::task<float> {
			co_return co_await []() -> lazy::task<int> { co_return 2; }();
		}();
		REQUIRE(v1 == 2.f);

		co_return v0 / v1;
	}()};

	REQUIRE(t.wait() == lazy::state::done);
	REQUIRE(t.has_result());
	REQUIRE(t.result() == 5.0);
}

//TODO: more complex timer test case
TEST_CASE("time", "[lazy]") {
	using namespace std::chrono_literals;

	auto t{[]() -> lazy::root_task<> {
		co_await []() -> lazy::task<> {
			std::this_thread::sleep_for(1ms);
			co_return;
		}();
	}()};

	REQUIRE(t.wait_for(0ms) == lazy::state::suspended);
	std::this_thread::sleep_for(10ms);
	REQUIRE(t.wait_for(0ms) == lazy::state::suspended);
	std::this_thread::sleep_for(10ms);
	REQUIRE(t.wait_for(0ms) == lazy::state::done);
	const auto elapsed{t.elapsed()};
	std::println("elapsed: {}", std::chrono::duration_cast<std::chrono::milliseconds>(elapsed));
	REQUIRE(elapsed < 2ms);
}

//TODO: more complex test case for mutex (including threads and thread migration)
TEST_CASE("mutex", "[lazy]") {
	using namespace std::chrono_literals;
	static lazy::mutex m;

	auto t0{[]() -> lazy::root_task<int> {
		co_return co_await m.locked([]() -> lazy::task<int> {
			std::println("t0 locked m");
			std::this_thread::sleep_for(10ms);
			co_yield lazy::progress;
			std::println("t0 unlocking m");
			co_return 10;
		}());
	}()};

	auto t1{[]() -> lazy::root_task<> {
		co_await m.locked([]() -> lazy::task<> {
			std::println("t1 locked m");
			co_yield lazy::progress;
			std::println("t1 unlocking m");
		}());
	}()};

	REQUIRE(t0.wait_for(1ms) == lazy::state::suspended);
	for(auto i{0}; i < 3; ++i) {
		REQUIRE(t1.wait_for(1ms) == lazy::state::blocked);
		std::println("t1 blocked");
	}
	REQUIRE(t0.wait() == lazy::state::done);
	REQUIRE(t0.has_result());
	REQUIRE(t0.result() == 10);

	REQUIRE(t1.wait() == lazy::state::done);
	REQUIRE(t1.has_result());
	t1.result();
}

//TODO: more complex logging test case
TEST_CASE("logging", "[lazy]") {
	auto t = [](auto) -> lazy::root_task<> {
		co_await lazy::warning("This is visible");
		co_await lazy::debug("This is invisible");
		co_await lazy::error("This is once again visible");
		throw std::logic_error{"This is an exception"};
	}(lazy::log_level::info);

	REQUIRE_THROWS(t.wait());
	REQUIRE(t.log().size() == 3);
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
	std::println("flipflop");
	for(int i = 0; i < 8; ++i) {
		co_yield lazy::progress;
		co_yield i % 2;
		co_yield lazy::progress;
	}
}

auto iota() -> lazy::generator<int> {
	co_yield lazy::elements_of{flipflop()};
	std::println("iota");
	for(int i = 0; i < 10; ++i) {
		co_yield i;
	}
}

auto fibonacci() -> lazy::generator<int> {
	co_yield lazy::elements_of{iota()};
	std::println("fibonacci");
	auto a = 0, b = 1;
	for (;;) {
		co_yield std::exchange(a, std::exchange(b, a + b));
	}
}

TEST_CASE("generator fib", "[generator]") {
	auto t = []() -> lazy::root_task<void> {
		auto gen{fibonacci()};
		for(auto beg = co_await gen.begin(); beg != gen.end(); co_await ++beg) {
			auto && i{*beg};

			if(i > 1000) break;
			std::print("{} ", i);

			co_await []() -> lazy::task<void> {
				std::println("nested task");
				co_return;
			}(); 

			auto g{yolo()};
			for(auto b = co_await g.begin(); b != g.end(); co_await ++b) std::print("{}", *b);
			std::println();
		}

		std::println("{}", (co_await []() -> lazy::task<std::string> { co_return "=========== DONE ==========="; }()).c_str());
	}();


#if 0
	using namespace std::chrono_literals;
	while(not t.wait_for(0ms)) std::print(" ===== ");
#else
	REQUIRE(t.wait() == lazy::state::done);
#endif
}

TEST_CASE("generator elements_of", "[generator]") {
	auto t{[] -> lazy::root_task<void> {
		auto outer{[] -> lazy::generator<std::string_view> {
			co_yield "before";
			auto inner{[] -> lazy::generator<std::string_view> {
				for(auto i{0}; i < 5; ++i)
					co_yield std::format("test {}", i);
			}()};
			co_yield lazy::elements_of{std::move(inner)};
			co_yield "after";
		}()};
		for(auto it{co_await outer.begin()}; it != outer.end(); co_await ++it)
			std::println("{}", *it);
	}()};
	REQUIRE(t.wait() == lazy::state::done);
}

TEST_CASE("generator move-only", "[generator]") {
	struct move_only {
		move_only() {}
		move_only(const move_only &) =delete;
		move_only(move_only &&) noexcept {}
		auto operator=(const move_only &) -> move_only & =delete;
		auto operator=(move_only &&) noexcept -> move_only & { return *this; }
		~move_only() noexcept {}
	};

	auto t{[] -> lazy::root_task<void> {
		auto g{[] -> lazy::generator<move_only> {
			co_yield move_only{};
			move_only mo;
			//error: co_yield mo;
		}()};
		for(auto it{co_await g.begin()}; it != g.end(); co_await ++it) {
			(void)*it;
		}
	}()};
}





