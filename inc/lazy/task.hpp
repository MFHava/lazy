
//          Copyright Michael Florian Hava.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file ../../LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#pragma once
#include <chrono>
#include <utility>
#include <optional>
#include <coroutine>
#include <type_traits>

namespace lazy {
	namespace internal {
		struct progress_t final {
			constexpr
			explicit
			progress_t(int) noexcept {}
		};

		struct function_ref final {
			const void * ctx;
			bool (*fptr)(const void *) noexcept;
		};

		struct promise_base;

		struct nested_info final {
			std::exception_ptr eptr;        //needed for manual stack unwinding
			std::coroutine_handle<> parent; //directly preceding coroutine
			promise_base * root;            //bottom of implicit coroutine-"stack"
		};

		struct promise_base {
			std::coroutine_handle<> top; //top of implicit stack
			nested_info * nested{nullptr};
			function_ref * suspend{nullptr}; //callback to check for suspension on co_yield  - nullptr => never suspend

			auto must_suspend() const -> bool /*TODO: [C++26] pre(not nested)*/ { return suspend ? suspend->fptr(suspend->ctx) : false; }
		};

		template<typename T>
		struct basic_promise : promise_base {
			static_assert(std::is_object_v<T> and std::is_same_v<std::decay_t<T>, T>); //disqualify cv T &, T[], ...

			//! @note result of computation, only set once task is done
			std::optional<T> result;

			template<typename U = T>
			void return_value(U && value) { result.emplace(std::forward<U>(value)); }
		};

		template<>
		struct basic_promise<void> : promise_base {
			static
			void return_void() noexcept {}
		};
	}

	//! @brief tag to yield progress within a @c task
	inline
	constexpr
	internal::progress_t progress{1};

	//! @brief cooperative synchronous(!) recursive coroutine task
	//! supported coroutine statements:
	//!  * @code{.cpp} co_yield progress; @endcode to yield control back from the coroutine to the caller
	//!  * @code{.cpp} [val =] co_await task; @endcode block this task until the awaited task is completed, optionally receiving a value
	//!  * @code{.cpp} co_return [val]; @endcode to terminate the task and optionally return a value to the caller
	template<typename T> //TODO: allocator support, default argument for T?
	struct task final {
		struct promise_type final : internal::basic_promise<T> {
			promise_type() { this->top = std::coroutine_handle<promise_type>::from_promise(*this); }

			auto get_return_object() noexcept { return task{std::coroutine_handle<promise_type>::from_promise(*this)}; }

			static
			auto initial_suspend() noexcept -> std::suspend_always { return {}; }

			static
			auto final_suspend() noexcept {
				struct awaiter final {
					static
					auto await_ready() noexcept { return false; }
					static
					auto await_suspend(std::coroutine_handle<promise_type> self) noexcept -> std::coroutine_handle<> {
						if(const auto nested{self.promise().nested}) {
							nested->root->top = nested->parent;
							if(not nested->root->must_suspend()) return nested->root->top;
						}
						return std::noop_coroutine();
					}
					static
					void await_resume() noexcept {}
				};
				return awaiter{};
			}

			auto yield_value(internal::progress_t) const noexcept {
				struct awaiter final {
					const bool suspend;

					auto await_ready() const noexcept { return not suspend; }
					static
					void await_suspend(std::coroutine_handle<>) noexcept {}
					static
					void await_resume() noexcept {}
				};
				return awaiter{(this->nested ? this->nested->root : this)->must_suspend()};
			}

			void unhandled_exception() const {
				if(this->nested) this->nested->eptr = std::current_exception();
				else throw;
			}

			template<typename U>
			static
			auto await_transform(task<U> && other) /*TODO: [C++26] pre(not other.valueless())*/ {
				struct awaiter final {
					task<U> other;
					internal::nested_info n;

					auto await_ready() const noexcept { return other.handle.done(); }
					auto await_suspend(std::coroutine_handle<promise_type> self) noexcept -> std::coroutine_handle<> {
						other.handle.promise().nested = std::addressof(n);
						n.parent = self;
						auto nested{self.promise().nested};
						(n.root = nested ? nested->root : std::addressof(self.promise()))->top = other.handle;
						return n.root->must_suspend() ? std::noop_coroutine() : n.root->top;
					}
					auto await_resume() const -> std::add_rvalue_reference_t<U> /*TODO: [C++26] pre(other.handle.done())*/ {
						if(n.eptr) std::rethrow_exception(n.eptr);
						if constexpr(not std::is_void_v<U>) return std::move(*other.handle.promise().result);
					}
				};
				return awaiter{std::move(other)};
			}
		};

		auto valueless() const noexcept -> bool { return !handle; }

		void wait() /*TODO: [C++26] pre(not valueless()) post(handle.done())*/ { if(not handle.done()) resume(handle.promise().top); }

		template<typename Rep, typename Period>
		auto wait_for(const std::chrono::duration<Rep, Period> & duration) -> bool /*TODO: [C++26] pre(not valueless())*/ { return wait_until(std::chrono::steady_clock::now() + duration); }

		template<typename Clock, typename Duration>
		auto wait_until(const std::chrono::time_point<Clock, Duration> & time) -> bool /*TODO: [C++26] pre(not valueless())*/ {
#if __cpp_lib_chrono >= 201907L
			static_assert(std::chrono::is_clock_v<Clock>);
#endif
			if(handle.done()) return true;
			auto & promise{handle.promise()};
			using Time = std::remove_reference_t<decltype(time)>;
			internal::function_ref s{.ctx = std::addressof(time), .fptr = +[](const void * ptr) noexcept { return Clock::now() >= *reinterpret_cast<Time *>(ptr); }};
			promise.suspend = &s;
			resume(promise.top);
			promise.suspend = nullptr;
			return handle.done();
		}

		auto get() -> std::add_lvalue_reference_t<T> /*TODO: [C++26] pre(not valueless()) post(handle.done())*/ {
			wait();
			if constexpr(not std::is_void_v<T>) return *handle.promise().result;
		}

		task(task && other) noexcept : handle{std::exchange(other.handle, {})} {}
		auto operator=(task && other) noexcept -> task & {
			std::swap(handle, other.handle);
			return *this;
		}
		~task() noexcept { if(handle) handle.destroy(); }
	private:
		friend promise_type;

		template<typename U>
		friend
		struct task;

		task(std::coroutine_handle<promise_type> handle) noexcept : handle{handle} {}

		void resume(std::coroutine_handle<> top) /*TODO: [C++26] pre(top and not top.done())*/ {
			try { top.resume(); }
			catch(...) {
				std::exchange(handle, {}).destroy(); //! @attention mark @c *this as @c valueless to trigger precondition violations on future usage
				throw;
			}
		}

		std::coroutine_handle<promise_type> handle;
	};
}

