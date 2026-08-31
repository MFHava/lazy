
//		  Copyright Michael Florian Hava.
// Distributed under the Boost Software License, Version 1.0.
//	(See accompanying file ../LICENSE_1_0.txt or copy at
//		  http://www.boost.org/LICENSE_1_0.txt)

#include <lazy/lazy.hpp>
#include <thread>
#include <catch2/catch_test_macros.hpp>

namespace {
	template<typename... Lambdas>
	struct lambda_visitor : Lambdas... {
		using Lambdas::operator()...;
	};

	struct move_only final {
		move_only() {}
		move_only(const move_only &) =delete;
		move_only(move_only &&) noexcept {}
		auto operator=(const move_only &) -> move_only & =delete;
		auto operator=(move_only &&) noexcept -> move_only & { return *this; }
		~move_only() noexcept {}
	};

	class wait_for final : public lazy::internal::yield_base {
		const std::chrono::milliseconds time;
	public:
		wait_for(std::chrono::milliseconds ms) noexcept : time{ms} {}

		template<typename Promise>
		auto await_suspend(std::coroutine_handle<Promise> self) const noexcept -> bool {
			std::this_thread::sleep_for(time);
			return self.promise().find_root().suspend();
		}
	};
}
using namespace std::chrono_literals;

//! @defgroup task Task
//! @{
TEST_CASE("task not valueless", "[task]") {
	lazy::root r{[] -> lazy::task<int> { co_return 1; }()};
	REQUIRE(not r.valueless());

	REQUIRE(r.wait() == lazy::state::done);
	REQUIRE(not r.valueless());

	REQUIRE(r.result() == 1);
	REQUIRE(not r.valueless());
}

TEST_CASE("task not valueless after exception", "[task]") {
	lazy::root r{[] -> lazy::task<> {
		throw 0;
		co_return;
	}()};
	REQUIRE(not r.valueless());
	REQUIRE_THROWS(r.wait());
	REQUIRE(not r.valueless());
}

TEST_CASE("task nesting", "[task]") {
	lazy::root r{[] -> lazy::task<double> {
		auto t0 = co_await [] -> lazy::task<int> { co_return 10; }();
		REQUIRE(t0 == 10);

		auto t1 = co_await [] -> lazy::task<float> {
			co_return co_await [] -> lazy::task<int> { co_return 2; }();
		}();
		REQUIRE(t1 == 2.f);

		co_return t0 / t1;
	}()};

	REQUIRE(r.wait() == lazy::state::done);
	REQUIRE(r.result() == 5.0);
}

TEST_CASE("task move-only result", "[task]") {
	lazy::root r0{[] -> lazy::task<move_only> { co_return move_only{}; }()};

	REQUIRE(r0.wait() == lazy::state::done);
	REQUIRE(r0.result());

	lazy::root r1{[] -> lazy::task<move_only> {
		move_only mo;
		co_return mo;
	}()};

	REQUIRE(r1.wait() == lazy::state::done);
	REQUIRE(r1.result());
}

TEST_CASE("task timed void", "[task]") {
	lazy::root r{[] -> lazy::task<lazy::duration> {
		co_return co_await lazy::timed{[] -> lazy::task<> {
			co_yield wait_for{5ms};
		}()};
	}()};

	REQUIRE(r.wait() == lazy::state::done);
	REQUIRE(r.result());
	REQUIRE(r.result() < r.elapsed());
}

TEST_CASE("task timed result", "[task]") {
	lazy::root r{[] -> lazy::task<std::pair<lazy::duration, int>> {
		co_return co_await lazy::timed{[] -> lazy::task<int> {
			co_yield wait_for{5ms};
			co_return 1234;
		}()};
	}()};

	REQUIRE(r.wait() == lazy::state::done);
	REQUIRE(r.result());
	auto && [elapsed, result]{*r.result()};
	REQUIRE(elapsed < r.elapsed());
	REQUIRE(result == 1234);
}
//! @}

//! @defgroup generator Generator
//! @{
TEST_CASE("generator not valueless", "[generator]") {
}

TEST_CASE("generator not valueless after exception", "[generator]") {
	lazy::root r{[] -> lazy::generator<int> {
		throw 0;
		co_return;
	}()};
	REQUIRE(not r.valueless());
	REQUIRE_THROWS(r.wait());
	REQUIRE(not r.valueless());
}

TEST_CASE("generator nesting", "[generator]") {
	lazy::root r{[] -> lazy::generator<double> {
		auto g0{[] -> lazy::generator<int> { co_yield 10; }()};
		auto g1{[] -> lazy::generator<float> {
			auto g2{[] -> lazy::generator<int> { co_yield 2; }()};
			for(auto it{co_await g2.begin()}; it != g2.end(); co_await ++it)
				co_yield *it;
		}()};
		for(auto it{co_await g0.begin()}; it != g0.end(); co_await ++it) {
			auto t0{*it};
			REQUIRE(t0 == 10);
			for(auto itt{co_await g1.begin()}; itt != g1.end(); co_await ++itt) {
				auto t1{*itt};
				REQUIRE(t1 == 2.f);
				co_yield t0 / t1;
			}
		}
	}()};

	REQUIRE(r.wait() == lazy::state::suspended);
	REQUIRE(r.result() == 5.0);
	REQUIRE(r.wait() == lazy::state::done);
	REQUIRE(not r.result());
}

TEST_CASE("generator move-only result", "[generator]") {
	lazy::root t{[] -> lazy::generator<move_only> {
		co_yield move_only{};
		move_only mo;
		co_yield std::move(mo);
	}()};

	REQUIRE(t.wait() == lazy::state::suspended);
	REQUIRE(t.result());
	REQUIRE(t.wait() == lazy::state::suspended);
	REQUIRE(t.result());
	REQUIRE(t.wait() == lazy::state::done);
	REQUIRE(not t.result());
}

TEST_CASE("generator elements_of", "[generator]") {
	lazy::root r{[] -> lazy::generator<int> {
		auto g{[] -> lazy::generator<int> {
			for(auto i{0}; i < 5; ++i)
				co_yield i;

			co_yield lazy::elements_of{[] -> lazy::generator<int> {
				for(auto i{5}; i < 10; ++i)
					co_yield i;
			}()};
		}()};
		co_yield lazy::elements_of{std::move(g)};
	}()};

	for(auto i{0}; i < 10; ++i) {
		REQUIRE(r.wait() == lazy::state::suspended);
		REQUIRE(r.result() == i);
	}
	REQUIRE(r.wait() == lazy::state::done);
}
//! @}

//! @defgroup all_of Waiting For Many Tasks
//! @{
TEST_CASE("all_of compile-time void", "[all_of]") {
	lazy::root r{lazy::all_of(
		[] -> lazy::task<> { co_return; }(),
		[] -> lazy::task<> { co_return; }()
	)};

	REQUIRE(r.wait() == lazy::state::done);
}

TEST_CASE("all_of runtime void", "[all_of]") {
	std::vector<lazy::task<>> tasks;
	for(auto i{0}; i < 2; ++i) tasks.emplace_back([] -> lazy::task<> { co_return; }());
	lazy::root r{lazy::all_of(std::move(tasks))};

	REQUIRE(r.wait() == lazy::state::done);
}

TEST_CASE("all_of compile-time single", "[all_of]") {
	lazy::root r{lazy::all_of(
		[] -> lazy::task<> { co_return; }(),
		[] -> lazy::task<int> { co_return 100; }(),
		[] -> lazy::task<> { co_return; }()
	)};

	REQUIRE(r.wait() == lazy::state::done);
	REQUIRE(r.result() == 100);
}

TEST_CASE("all_of compile-time multiple", "[all_of]") {
	lazy::root r{lazy::all_of(
		[] -> lazy::task<> { co_return; }(),
		[] -> lazy::task<int> { co_return 100; }(),
		[] -> lazy::task<> { co_return; }(),
		[] -> lazy::task<double> { co_return 3.14; }(),
		[] -> lazy::task<> { co_return; }()
	)};

	REQUIRE(r.wait() == lazy::state::done);
	REQUIRE(r.result());
	auto & result{*r.result()};
	static_assert(std::is_same_v<std::decay_t<decltype(result)>, std::tuple<int, double>>);

	auto & [a, b]{result};
	REQUIRE(a == 100);
	REQUIRE(b == 3.14);
}

TEST_CASE("all_of counted compile-time", "[all_of]") {
	lazy::root r{lazy::all_of(lazy::cw<3>, [](auto i) -> lazy::task<decltype(i)> { co_return i; })};

	REQUIRE(r.wait() == lazy::state::done);
	REQUIRE(r.result());
	auto & [a, b, c]{*r.result()};
	REQUIRE(a == 0);
	REQUIRE(b == 1);
	REQUIRE(c == 2);
}

TEST_CASE("all_of counted runtime", "[all_of]") {
	lazy::root r{lazy::all_of(3, [](auto i) -> lazy::task<decltype(i)> { co_return i; })};

	REQUIRE(r.wait() == lazy::state::done);
	REQUIRE(r.result());
	auto & result{*r.result()};

	REQUIRE(result.size()== 3);
	REQUIRE(result[0] == 0);
	REQUIRE(result[1] == 1);
	REQUIRE(result[2] == 2);
}

TEST_CASE("all_of compile-time throw", "[all_of]") {
	lazy::root r{lazy::all_of(
		[] -> lazy::task<int> { co_return 10; }(),
		[] -> lazy::task<double> { co_return 3.14; }(),
		[] -> lazy::task<void> { throw std::bad_exception{}; co_return; }()
	)};

	REQUIRE_THROWS_AS(r.wait(), std::bad_exception);
}

TEST_CASE("all_of runtime throw", "[all_of]") {
	std::vector<lazy::task<int>> tasks;
	tasks.emplace_back([] -> lazy::task<int> { co_return 5; }());
	tasks.emplace_back([] -> lazy::task<int> { co_return 3; }());
	tasks.emplace_back([] -> lazy::task<int> { co_return 1; }());
	tasks.emplace_back([] -> lazy::task<int> { throw std::bad_exception{}; co_return 0; }());

	lazy::root r{lazy::all_of(std::move(tasks))};
	REQUIRE_THROWS_AS(r.wait(), std::bad_exception);
}

TEST_CASE("all_of compile-time blocked", "[all_of]") {
	lazy::root r{lazy::all_of(lazy::cw<2>, [](auto i) -> lazy::task<> {
		if(i == 0) co_yield lazy::progress;
		else co_yield lazy::blocked;
		co_yield lazy::progress;
		co_yield lazy::blocked;
		co_yield lazy::progress;
	})};

	REQUIRE(r.wait_for(0ms) == lazy::state::suspended);
	REQUIRE(r.wait_for(0ms) == lazy::state::suspended);
	REQUIRE(r.wait_for(0ms) == lazy::state::blocked);
	REQUIRE(r.wait_for(0ms) == lazy::state::suspended);
	REQUIRE(r.wait() == lazy::state::done);
}

TEST_CASE("all_of runtime blocked", "[all_of]") {
	lazy::root r{lazy::all_of(2, [](auto i) -> lazy::task<> {
		if(i == 0) co_yield lazy::progress;
		else co_yield lazy::blocked;
		co_yield lazy::progress;
		co_yield lazy::blocked;
		co_yield lazy::progress;
	})};

	REQUIRE(r.wait_for(0ms) == lazy::state::suspended);
	REQUIRE(r.wait_for(0ms) == lazy::state::suspended);
	REQUIRE(r.wait_for(0ms) == lazy::state::blocked);
	REQUIRE(r.wait_for(0ms) == lazy::state::suspended);
	REQUIRE(r.wait() == lazy::state::done);
}
//! @}

//! @defgroup any_of Waiting For One of Many Tasks
//! @{
TEST_CASE("any_of compile-time", "[any_of]") {
	lazy::root r{lazy::any_of(lazy::cw<true>,
		[] -> lazy::task<int> {
			co_yield wait_for(1ms);
			co_return 0;
		}(),
		[] -> lazy::task<int> {
			co_return co_await [] -> lazy::task<int> {
				co_yield wait_for(1ms);
				co_yield wait_for(1ms);
				co_return 1;
			}();
		}(),
		[] -> lazy::task<int> {
			co_return 2;
		}()
	)};

	REQUIRE(r.wait() == lazy::state::suspended);
	REQUIRE(r.result() == 2);
	REQUIRE(r.wait() == lazy::state::suspended);
	REQUIRE(r.result() == 0);
	REQUIRE(r.wait() == lazy::state::suspended);
	REQUIRE(r.result() == 1);
	REQUIRE(r.wait() == lazy::state::done);
}

TEST_CASE("any_of runtime", "[any_of]") {
	std::vector<lazy::task<int>> tasks;
	tasks.emplace_back([] -> lazy::task<int> {
		co_yield wait_for(1ms);
		co_return 0;
	}());
	tasks.emplace_back([] -> lazy::task<int> {
		co_return co_await [] -> lazy::task<int> {
			co_yield wait_for(1ms);
			co_yield wait_for(1ms);
			co_return 1;
		}();
	}());
	tasks.emplace_back([] -> lazy::task<int> {
		co_return 2;
	}());

	lazy::root r{lazy::any_of(lazy::cw<true>, std::move(tasks))};

	REQUIRE(r.wait() == lazy::state::suspended);
	REQUIRE(r.result() == 2);
	REQUIRE(r.wait() == lazy::state::suspended);
	REQUIRE(r.result() == 0);
	REQUIRE(r.wait() == lazy::state::suspended);
	REQUIRE(r.result() == 1);
	REQUIRE(r.wait() == lazy::state::done);
}

TEST_CASE("any_of compile-time int, int", "[any_of]") {
	lazy::root r{lazy::any_of(lazy::cw<true>,
		[] -> lazy::task<int> { co_return 10; }(),
		[] -> lazy::task<int> { co_return 100; }()
	)};

	REQUIRE(r.wait() == lazy::state::suspended);
	if(r.result() == 10) {
		REQUIRE(r.wait() == lazy::state::suspended);
		REQUIRE(r.result() == 100);
	} else {
		REQUIRE(r.result() == 100);
		REQUIRE(r.wait() == lazy::state::suspended);
		REQUIRE(r.result() == 10);
	}
	REQUIRE(r.wait() == lazy::state::done);
}

TEST_CASE("any_of compile-time int, double", "[any_of]") {
	lazy::root r{lazy::any_of(lazy::cw<true>,
		[] -> lazy::task<int> { co_return 100; }(),
		[] -> lazy::task<double> { co_return 3.14; }()
	)};

	REQUIRE(r.wait() == lazy::state::suspended);
	std::visit(lambda_visitor{
		[&](int val) {
			REQUIRE(val == 100);
			REQUIRE(r.wait() == lazy::state::suspended);
			REQUIRE(std::get<double>(*r.result()) == 3.14);
		},
		[&](double val) {
			REQUIRE(val == 3.14);
			REQUIRE(r.wait() == lazy::state::suspended);
			REQUIRE(std::get<int>(*r.result()) == 100);
		}
	}, *r.result());
	REQUIRE(r.wait() == lazy::state::done);
}

TEST_CASE("any_of counted compile-time", "[any_of]") {
	lazy::root r{lazy::any_of(lazy::cw<true>, lazy::cw<3>, [](auto i) -> lazy::task<decltype(i)> { co_return i; })};
	static_assert(std::is_same_v<std::decay_t<decltype(*r.result())>, std::size_t>);

	std::vector<std::size_t> tmp;
	while(r.wait() != lazy::state::done) tmp.emplace_back(*r.result());
	std::ranges::sort(tmp);
	REQUIRE(std::ranges::equal(tmp, std::array{0, 1, 2}));
}

TEST_CASE("any_of counted runtime", "[any_of]") {
	lazy::root r{lazy::any_of(lazy::cw<true>, 3, [](auto i) -> lazy::task<decltype(i)> { co_return i; })};
	static_assert(std::is_same_v<std::decay_t<decltype(*r.result())>, std::size_t>);

	std::vector<std::size_t> tmp;
	while(r.wait() != lazy::state::done) tmp.emplace_back(*r.result());
	std::ranges::sort(tmp);
	REQUIRE(std::ranges::equal(tmp, std::array{0, 1, 2}));

}

TEST_CASE("any_of compile-time throw ignored without any results", "[any_of]") {
	lazy::root r{lazy::any_of(lazy::cw<true>,
		[] -> lazy::task<int> {
			throw std::logic_error{"This shouldn't propagate"};
			co_return 0;
		}(),
		[] -> lazy::task<int> {
			co_yield lazy::progress;
			co_yield wait_for{10ms};
			throw std::logic_error{"This shouldn't propagagte either"};
			co_return 0;
		}()
	)};

	REQUIRE(r.wait() == lazy::state::done);
}

TEST_CASE("any_of compile-time throw ignored after result", "[any_of]") {
	lazy::root r{lazy::any_of(lazy::cw<true>,
		[] -> lazy::task<int> {
			throw std::logic_error{"This shouldn't propagate"};
			co_return 0;
		}(),
		[] -> lazy::task<int> {
			co_yield lazy::progress;
			co_yield wait_for{10ms};
			throw std::logic_error{"This shouldn't propagagte either"};
			co_return 0;
		}(),
		[] -> lazy::task<int> {
			co_yield lazy::progress;
			co_yield wait_for{10ms};
			co_yield wait_for{10ms};
			co_return 10;
		}()
	)};

	REQUIRE(r.wait() == lazy::state::suspended);
	REQUIRE(r.result() == 10);
	REQUIRE(r.wait() == lazy::state::done);
}

TEST_CASE("any_of runtime throw ignored without any results", "[any_of]") {
	std::vector<lazy::task<int>> tasks;
	tasks.emplace_back([] -> lazy::task<int> {
		throw std::logic_error{"This shouldn't propagate"};
		co_return 0;
	}());
	tasks.emplace_back([] -> lazy::task<int> {
		co_yield lazy::progress;
		co_yield wait_for{10ms};
		throw std::logic_error{"This shouldn't propagagte either"};
		co_return 0;
	}());

	lazy::root r{lazy::any_of(lazy::cw<true>, std::move(tasks))};
	REQUIRE(r.wait() == lazy::state::done);
}

TEST_CASE("any_of runtime throw ignored after result", "[any_of]") {
	std::vector<lazy::task<int>> tasks;
	tasks.emplace_back([] -> lazy::task<int> {
		throw std::logic_error{"This shouldn't propagate"};
		co_return 0;
	}());
	tasks.emplace_back([] -> lazy::task<int> {
		co_yield lazy::progress;
		co_yield wait_for{10ms};
		throw std::logic_error{"This shouldn't propagagte either"};
		co_return 0;
	}());
	tasks.emplace_back([] -> lazy::task<int> {
		co_yield lazy::progress;
		co_yield wait_for{10ms};
		co_yield wait_for{10ms};
		co_return 10;
	}());

	lazy::root r{lazy::any_of(lazy::cw<true>, std::move(tasks))};
	REQUIRE(r.wait() == lazy::state::suspended);
	REQUIRE(r.result() == 10);
	REQUIRE(r.wait() == lazy::state::done);
}

TEST_CASE("any_of compile-time throw not ignored without any results", "[any_of]") {
	lazy::root r{lazy::any_of(lazy::cw<false>,
		[] -> lazy::task<int> {
			throw std::logic_error{"Either this should propagate"};
			co_return 0;
		}(),
		[] -> lazy::task<int> {
			co_yield lazy::progress;
			co_yield wait_for{10ms};
			throw std::logic_error{"Or this should propagagte"};
			co_return 0;
		}()
	)};

	REQUIRE_THROWS_AS(r.wait(), std::logic_error);
}

TEST_CASE("any_of compile-time throw not ignored after result", "[any_of]") {
	lazy::root r{lazy::any_of(lazy::cw<false>,
		[] -> lazy::task<int> {
			co_yield wait_for{10ms};
			throw std::logic_error{"This should propagate"};
			co_return 0;
		}(),
		[] -> lazy::task<int> {
			co_yield lazy::progress;
			co_yield wait_for{10ms};
			co_yield wait_for{10ms};
			throw std::runtime_error{"This shouldn't be reached"};
			co_return 0;
		}(),
		[] -> lazy::task<int> {
			co_return 10;
		}()
	)};

	REQUIRE(r.wait() == lazy::state::suspended);
	REQUIRE(r.result() == 10);
	REQUIRE_THROWS_AS(r.wait(), std::logic_error);
}

TEST_CASE("any_of runtime throw not ignored without any results", "[any_of]") {
	std::vector<lazy::task<int>> tasks;
	tasks.emplace_back([] -> lazy::task<int> {
		throw std::logic_error{"Either this should propagate"};
		co_return 0;
	}());
	tasks.emplace_back([] -> lazy::task<int> {
		co_yield lazy::progress;
		co_yield wait_for{10ms};
		throw std::logic_error{"Or this should propagagte either"};
		co_return 0;
	}());

	lazy::root r{lazy::any_of(lazy::cw<false>, std::move(tasks))};
	REQUIRE_THROWS_AS(r.wait(), std::logic_error);
}

TEST_CASE("any_of runtime throw not ignored after result", "[any_of]") {
	std::vector<lazy::task<int>> tasks;
	tasks.emplace_back([] -> lazy::task<int> {
		co_yield wait_for{10ms};
		throw std::logic_error{"This should propagate"};
		co_return 0;
	}());
	tasks.emplace_back([] -> lazy::task<int> {
		co_yield lazy::progress;
		co_yield wait_for{10ms};
		co_yield wait_for{10ms};
		throw std::runtime_error{"This shouldn't be reached"};
		co_return 0;
	}());
	tasks.emplace_back([] -> lazy::task<int> {
		co_return 10;
	}());

	lazy::root r{lazy::any_of(lazy::cw<false>, std::move(tasks))};
	REQUIRE(r.wait() == lazy::state::suspended);
	REQUIRE(r.result() == 10);
	REQUIRE_THROWS_AS(r.wait(), std::logic_error);
}

TEST_CASE("any_of compile-time blocked", "[any_of]") {
	lazy::root r{lazy::any_of(lazy::cw<true>, lazy::cw<2>, [](auto i) -> lazy::task<int> {
		if(i == 0) co_yield lazy::progress;
		else co_yield lazy::blocked;
		co_yield lazy::progress;
		co_yield lazy::blocked;
		co_yield lazy::progress;
		co_return 0;
	})};

	REQUIRE(r.wait_for(0ms) == lazy::state::suspended);
	REQUIRE(not r.result());
	REQUIRE(r.wait_for(0ms) == lazy::state::suspended);
	REQUIRE(not r.result());
	REQUIRE(r.wait_for(0ms) == lazy::state::blocked);
	REQUIRE(r.wait_for(0ms) == lazy::state::suspended);
	REQUIRE(not r.result());
	REQUIRE(r.wait() == lazy::state::suspended);
	REQUIRE(r.result() == 0);
	REQUIRE(r.wait() == lazy::state::suspended);
	REQUIRE(r.result() == 0);
	REQUIRE(r.wait() == lazy::state::done);
}

TEST_CASE("any_of runtime blocked", "[any_of]") {
	lazy::root r{lazy::any_of(lazy::cw<true>, 2, [](auto i) -> lazy::task<int> {
		if(i == 0) co_yield lazy::progress;
		else co_yield lazy::blocked;
		co_yield lazy::progress;
		co_yield lazy::blocked;
		co_yield lazy::progress;
		co_return 0;
	})};

	REQUIRE(r.wait_for(0ms) == lazy::state::suspended);
	REQUIRE(not r.result());
	REQUIRE(r.wait_for(0ms) == lazy::state::suspended);
	REQUIRE(not r.result());
	REQUIRE(r.wait_for(0ms) == lazy::state::blocked);
	REQUIRE(r.wait_for(0ms) == lazy::state::suspended);
	REQUIRE(not r.result());
	REQUIRE(r.wait() == lazy::state::suspended);
	REQUIRE(r.result() == 0);
	REQUIRE(r.wait() == lazy::state::suspended);
	REQUIRE(r.result() == 0);
	REQUIRE(r.wait() == lazy::state::done);
}
//@}

//! @defgroup mutex Synchronization Primitives
//! @{
TEST_CASE("mutex", "[mutex]") {
	lazy::mutex m;
	lazy::root r0{m.locked([] -> lazy::task<int> {
		co_yield wait_for{10ms};
		co_return 10;
	}())};
	lazy::root r1{m.locked([] -> lazy::task<> {
		co_yield lazy::progress;
	}())};

	REQUIRE(r0.wait_for(1ms) == lazy::state::suspended);
	for(auto i{0}; i < 3; ++i) REQUIRE(r1.wait_for(1ms) == lazy::state::blocked);
	REQUIRE(r0.wait() == lazy::state::done);
	REQUIRE(r0.result() == 10);
	REQUIRE(r1.wait() == lazy::state::done);
}

TEST_CASE("shared_mutex", "[shared_mutex]") {
	static lazy::shared_mutex m;
	lazy::root r0{[] -> lazy::task<> {
		co_await m.locked([] -> lazy::task<> {
			co_yield wait_for{10ms};
		}());
		co_yield wait_for(10ms);
		co_await m.locked([] -> lazy::task<> {
			co_yield wait_for{10ms};
		}());
	}()};
	lazy::root r1{m.shared_locked([] -> lazy::task<> {
		co_yield wait_for{10ms};
	}())};
	lazy::root r2{m.shared_locked([] -> lazy::task<> {
		co_yield wait_for{10ms};
	}())};

	REQUIRE(r0.wait_for(1ms) == lazy::state::suspended);
	for(auto i{0}; i < 3; ++i) {
		REQUIRE(r1.wait_for(1ms) == lazy::state::blocked);
		REQUIRE(r2.wait_for(1ms) == lazy::state::blocked);
	}
	REQUIRE(r0.wait_for(1ms) == lazy::state::suspended);
	REQUIRE(r1.wait_for(1ms) == lazy::state::suspended);
	REQUIRE(r2.wait_for(1ms) == lazy::state::suspended);
	REQUIRE(r0.wait_for(1ms) == lazy::state::blocked);
	REQUIRE(r1.wait_for(1ms) == lazy::state::done);
	REQUIRE(r2.wait_for(1ms) == lazy::state::done);
	REQUIRE(r0.wait() == lazy::state::done);
}
//! @}

//! @defgroup logging Logging Framework
//! @{
TEST_CASE("logging", "[logging]") {
	lazy::root r{lazy::log_level::info, [] -> lazy::task<> {
		int val{1234};
		co_await lazy::warning{"This is visible - {}", val};
		co_await lazy::debug{"This is invisible"};
		co_await lazy::error{"This is once again visible"};
		throw std::logic_error{"This is an exception"};
	}()};
	
	REQUIRE_THROWS(r.wait());
	REQUIRE(r.log().size() == 3);
	REQUIRE(r.log()[0].level == lazy::log_level::warning);
	REQUIRE(r.log()[1].level == lazy::log_level::error);
	REQUIRE(r.log()[2].level == lazy::log_level::fatal);
}

namespace {
	template<std::ranges::input_range R>
	class dump_range final : public lazy::dump_base {
		R & r;

		void dump_to(std::back_insert_iterator<std::string> out) const override {
			std::ranges::copy(r | std::views::transform([](auto v) { return std::to_string(v); }) | std::views::join, out);
		}
	public:
		dump_range(std::string_view name, R && r, std::source_location loc = std::source_location::current()) noexcept : lazy::dump_base{name, loc}, r{r} {}
	};

	template<typename R>
	dump_range(std::string_view, R &&) -> dump_range<R>;
}

TEST_CASE("dumping", "[logging]") {
	lazy::root r{[] -> lazy::task<> { co_await dump_range{"test.abc", std::views::iota(0) | std::views::take(5)}; }()};
	REQUIRE(r.wait() == lazy::state::done);
	REQUIRE(r.log().size() == 1);
	const std::string_view str{r.log().front().data};
	REQUIRE(str.size() == 14);
	const auto index{str.find('\0')};
	REQUIRE(str.substr(0, index) == "test.abc");
	REQUIRE(str.substr(index + 1) == "01234");
	REQUIRE(str == std::string_view{"test.abc\u{0}01234", 14});
}

TEST_CASE("is_tracing", "[logging]") {
	lazy::root r0{lazy::log_level::trace, [] -> lazy::task<> { REQUIRE(co_await lazy::get_is_tracing); }()};
	REQUIRE(r0.wait() == lazy::state::done);

	lazy::root r1{lazy::log_level::fatal, [] -> lazy::task<> { REQUIRE(not co_await lazy::get_is_tracing); }()};
	REQUIRE(r1.wait() == lazy::state::done);
}
//! @}
