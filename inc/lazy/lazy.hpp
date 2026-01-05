
//          Copyright Michael Florian Hava.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file ../../LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#pragma once
#include <chrono>
#include <ranges>
#include <utility>
#include <optional>
#include <coroutine>
#include <type_traits>

namespace lazy {
	template<typename>
	struct task;

	namespace internal {
		//! @brief internal accessor to handle
		auto get_handle(auto & val) noexcept { return val.handle; }

		struct progress_t final {
			constexpr
			explicit
			progress_t(int) noexcept {}
		};

		struct active_root final {
			std::coroutine_handle<> top;

			//! @note inlined @c function_ref
			const void * ctx;
			bool (*fptr)(const void *) noexcept;

			auto suspend() const noexcept -> bool { return fptr(ctx); }
		};

		class promise_base {
			struct nested_info final {
				std::exception_ptr eptr;        //needed for manual stack unwinding
				std::coroutine_handle<> parent; //directly preceding coroutine
				promise_base * root;            //bottom of implicit coroutine-"stack"
			};

			auto get_nested() const -> nested_info * { return (data & 1U) ? reinterpret_cast<nested_info *>(data ^ 1U) : nullptr; }
			void set_nested(nested_info & nested) /*TODO: [C++26] post(not (data & 1U))*/ { data = reinterpret_cast<std::uintptr_t>(&nested) | 1U; } 
		public:
			//! @attention tagged "union"
			//! LSB set => nested_info *
			//! if promise is at bottom of coroutine-"stack" => @c active_root*
			//! else @c void* obtained from @c std::coroutine_handle<>::address of top-coroutine
			std::uintptr_t data;

			static
			auto initial_suspend() noexcept -> std::suspend_always { return {}; }
			static
			auto final_suspend() noexcept { return pop_awaiter{}; }

			void unhandled_exception() {
				if(auto n{this->get_nested()}) n->eptr = std::current_exception();
				else throw;
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

				auto nested{get_nested()};
				auto ar{reinterpret_cast<active_root *>(nested ? nested->root->data : data)};
				return awaiter{ar->suspend()};
			}

			template<typename T>
			static
			auto await_transform(task<T> other) /*TODO: [C++26] pre(not other.valueless())*/ {
				struct awaiter : push_awaiter<task<T>> {
					auto await_resume() const -> std::add_rvalue_reference_t<T> /*TODO: [C++26] pre(other.handle.done())*/ {
						push_awaiter<task<T>>::await_resume();
						if constexpr(not std::is_void_v<T>) return std::move(*internal::get_handle(this->other).promise().result);
					}
				};
				return awaiter{std::move(other)};
			}

			template<typename Other, bool Initial>
			struct iterator_awaiter final { //TODO: add contracts and constraints / static_asserts
				Other other;
				nested_info n;
				std::coroutine_handle<> prev_top;

				auto await_ready() const noexcept { return get_handle(other).done(); }

				template<typename Promise>
				auto await_suspend(std::coroutine_handle<Promise> self) noexcept -> std::coroutine_handle<> {
					auto & other_promise{get_handle(other).promise()};

					//! @attention connect @c other 's @c co_yield with current coroutine frame
					if constexpr(Initial) other_promise.yield_target = self;
					//TODO: [C++26] else contract_assert(other_promise.yield_target == self);

					//! @attention store enough context to remove @c other from stack on resumption (as @c generator is not permanently on top of stack)
					prev_top = self;

					//! @attention push @c other (which contrary to normal push could already be nested ...) onto stack
					n.parent = self;

					const auto & nested{self.promise().get_nested()};
					n.root = nested ? nested->root : std::addressof(self.promise());
					auto ar{reinterpret_cast<active_root *>(n.root->data)};
					ar->top = std::coroutine_handle<>::from_address(reinterpret_cast<void *>(other_promise.data));
					other_promise.set_nested(n);

					return ar->suspend() ? std::noop_coroutine() : ar->top;
				}

				auto await_resume() {
					//! @note must be checked first, because if we got here via an unhandled exception, there is nothing to do apart from rethrowing
					if(n.eptr) std::rethrow_exception(n.eptr);

					auto other_handle{get_handle(other)};
					auto & other_promise{other_handle.promise()};
					auto ar{reinterpret_cast<active_root *>(n.root->data)};

					//! @attention @c other_promise.top won't be up to date, need to get actual top from @c *top so we can resume the correct coroutine on the next iteration
					other_promise.data = reinterpret_cast<std::uintptr_t>(ar->top.address());

					//! @attention due to type-erasure we can't get the correct @c ptr from @c top => copy said pointer to the "root" (only valid if resumption was due to yield)
					if(not other_handle.done()) other_promise.ptr = other_handle.from_address(reinterpret_cast<void *>(other_promise.data)).promise().ptr;

					//! @attention pop @c other from stack by restoring the @c top we had on @c await_suspend
					ar->top = prev_top;

					if constexpr(Initial) return std::move(other);
				}
			};

			template<typename T, bool U>
			static
			auto await_transform(iterator_awaiter<T, U> other) { return other; }
		private:
			struct pop_awaiter final { //TODO: add contracts and constraints / static_asserts
				static
				auto await_ready() noexcept { return false; }

				template<typename Promise>
				static
				auto await_suspend(std::coroutine_handle<Promise> self) noexcept -> std::coroutine_handle<> {
					if(const auto nested{self.promise().get_nested()}) {
						auto ar{reinterpret_cast<active_root *>(nested->root->data)};
						ar->top = nested->parent;
						if(not ar->suspend()) return ar->top;
					}
					return std::noop_coroutine();
				}

				static
				void await_resume() noexcept {}
			};
		protected:
			template<typename Other>
			struct push_awaiter { //TODO: add contracts and constraints / static_asserts
				Other other;
				nested_info n;

				auto await_ready() const noexcept { return get_handle(other).done(); }

				template<typename Promise>
				auto await_suspend(std::coroutine_handle<Promise> self) noexcept -> std::coroutine_handle<> {
					get_handle(other).promise().set_nested(n);
					n.parent = self;
					auto nested{self.promise().get_nested()};
					n.root = nested ? nested->root : std::addressof(self.promise());
					auto ar{reinterpret_cast<active_root *>(n.root->data)};
					ar->top = get_handle(other);
					return ar->suspend() ? std::noop_coroutine() : ar->top;
				}

				auto await_resume() const /*TODO: [C++26] pre(other.handle.done())*/ {
					if(n.eptr) std::rethrow_exception(n.eptr);
				}
			};
		};

		template<typename T>
		struct task_promise : promise_base {
			//! @note result of computation, only set once task is done
			std::optional<T> result;

			template<typename U = T>
			void return_value(U && value) { result.emplace(std::forward<U>(value)); }
		};

		template<>
		struct task_promise<void> : promise_base {
			static
			void return_void() noexcept {}
		};
	}

	//! @brief tag to yield progress within a @c task or @c generator
	inline
	constexpr
	internal::progress_t progress{1};

	//! @brief cooperative synchronous(!) recursive coroutine task
	//! @tparam Result return type of the task
	//! supported coroutine statements:
	//!  * @code{.cpp} co_yield progress; @endcode to yield control back from the coroutine to the caller
	//!  * @code{.cpp} [val =] co_await task; @endcode block this task until the awaited @c task is completed, optionally receiving a value
	//!  * @code{.cpp} for co_await(<type> val : gen) { ... } @endcode block this task until awaited generator yields next value
	//!  * @code{.cpp} co_return [val]; @endcode to terminate the task and optionally return a value to the caller
	template<typename Result = void>
	struct task final {
		static_assert(std::is_void_v<Result> or (std::is_object_v<Result> and std::is_same_v<std::decay_t<Result>, Result> and not std::is_same_v<std::decay_t<Result>, internal::progress_t>));

		struct promise_type final : internal::task_promise<Result> {
			promise_type() { this->data = reinterpret_cast<std::uintptr_t>(std::coroutine_handle<promise_type>::from_promise(*this).address()); }

			auto get_return_object() noexcept { return task{std::coroutine_handle<promise_type>::from_promise(*this)}; }
		};

		auto valueless() const noexcept -> bool { return not handle; }

		void wait() /*TODO: [C++26] pre(not valueless()) post(handle.done())*/ { if(not handle.done()) resume({.fptr = [](const void *) noexcept { return false; }}); }

		template<typename Rep, typename Period>
		auto wait_for(const std::chrono::duration<Rep, Period> & duration) -> bool /*TODO: [C++26] pre(not valueless())*/ { return wait_until(std::chrono::steady_clock::now() + duration); }

		template<typename Clock, typename Duration>
		auto wait_until(const std::chrono::time_point<Clock, Duration> & time) -> bool /*TODO: [C++26] pre(not valueless())*/ {
#if __cpp_lib_chrono >= 201907L
			static_assert(std::chrono::is_clock_v<Clock>);
#endif
			if(handle.done()) return true;
			resume({.ctx = std::addressof(time), .fptr = +[](const void * ptr) noexcept { return Clock::now() >= *reinterpret_cast<std::remove_reference_t<decltype(time)> *>(ptr); }});
			return handle.done();
		}

		auto get() -> std::add_lvalue_reference_t<Result> /*TODO: [C++26] pre(not valueless()) post(handle.done())*/ {
			wait();
			if constexpr(not std::is_void_v<Result>) return *handle.promise().result;
		}

		task(task && other) noexcept : handle{std::exchange(other.handle, {})} {}
		auto operator=(task && other) noexcept -> task & {
			std::swap(handle, other.handle);
			return *this;
		}
		~task() noexcept { if(handle) handle.destroy(); }
	private:
		friend
		auto internal::get_handle(auto &) noexcept;

		task(std::coroutine_handle<promise_type> handle) noexcept : handle{handle} {}

		void resume(internal::active_root ar) {
			try {
				auto & promise{handle.promise()};
				ar.top = std::coroutine_handle<>::from_address(reinterpret_cast<void *>(promise.data));
				//TODO: [C++26] contract_assert(ar.top and not ar.top.done());
				promise.data = reinterpret_cast<std::uintptr_t>(std::addressof(ar));
				ar.top.resume();
				promise.data = reinterpret_cast<std::uintptr_t>(ar.top.address());
			} catch(...) {
				std::exchange(handle, {}).destroy(); //! @attention mark @c *this as @c valueless to trigger precondition violations on future usage
				throw;
			}
		}

		std::coroutine_handle<promise_type> handle;
	};


	//! @brief lazy view of elements yielded by a coroutine
	//! @tparam Reference reference type of generator
	//! @tparam Value value type of the generator
	//! supported coroutine statements:
	//!  * @code{.cpp} co_yield progress; @endcode to yield control back from the coroutine to the caller
	//!  * @code{.cpp} co_yield val; @endcode yield value to caller of generator
	//!  * @code{.cpp} [val =] co_await task; @endcode block this generator until the awaited @c task is completed, optionally receiving a value
	//!  * @code{.cpp} co_await generator; @endcode yield elements of @c generator
	//!  * @code{.cpp} for co_await(<type> val : gen) { ... } @endcode block this generatro until awaited generator yields next value
	template<typename Reference, typename Value = void> //TODO: remove Value?
	class generator final : public std::ranges::view_interface<generator<Reference, Value>> {
		static_assert(not std::is_same_v<std::decay_t<Reference>, internal::progress_t>);
		static_assert(not std::is_same_v<std::decay_t<Value>, internal::progress_t>);

		using value = std::conditional_t<std::is_void_v<Value>, std::remove_cvref_t<Reference>, Value>;
		static_assert(std::is_object_v<value> and std::is_same_v<std::remove_cvref_t<value>, value>);

		using reference = std::conditional_t<std::is_void_v<Value>, Reference &&, Reference>;
		static_assert(std::is_reference_v<reference> or (std::is_object_v<reference> and std::is_same_v<std::remove_cv_t<reference>, reference> and std::copy_constructible<reference>));

		using rref = std::conditional_t<std::is_reference_v<reference>, std::remove_reference_t<reference> &&, reference>;
		static_assert(std::common_reference_with<reference &&, value &>);
		static_assert(std::common_reference_with<reference &&, rref &&>);
		static_assert(std::common_reference_with<rref &&, const value &>);
	public:
		using yielded = std::conditional_t<std::is_reference_v<reference>, reference, const reference &>;

		struct promise_type final : internal::promise_base {
			std::add_pointer_t<yielded> ptr;
			std::coroutine_handle<> yield_target;

			promise_type() { this->data = reinterpret_cast<std::uintptr_t>(std::coroutine_handle<promise_type>::from_promise(*this).address()); }

			auto get_return_object() noexcept -> generator { return std::coroutine_handle<promise_type>::from_promise(*this); }

			using internal::promise_base::yield_value;

			auto yield_value(yielded val) noexcept {
				ptr = std::addressof(val);
				return yield_awaiter{};
			}

			auto yield_value(const std::remove_reference_t<yielded> & lval) requires std::is_rvalue_reference_v<yielded> and std::constructible_from<std::remove_cvref_t<yielded>, const std::remove_reference_t<yielded> &> {
				struct awaiter final : yield_awaiter {
					std::remove_cvref_t<yielded> val;

					auto await_suspend(std::coroutine_handle<promise_type> self) noexcept {
						self.promise().ptr = std::addressof(val);
						return yield_awaiter::await_suspend(self);
					}
				};
				return awaiter{{}, lval};
			}

			using internal::promise_base::await_transform;

			template<typename R, typename V>
			requires std::same_as<typename generator<R, V>::yielded, yielded>
			auto await_transform(generator<R, V> other) /*TODO: [C++26] pre(not other.valueless())*/ { //TODO: is this really better than using co_yield?
				other.handle.promise().yield_target = yield_target;
				return internal::promise_base::push_awaiter{std::move(other)};
			}

			void return_void() const noexcept {}
		private:
			struct yield_awaiter { //TODO: add contracts and constraints / static_asserts
				static
				auto await_ready() noexcept { return false; }
				//! @note does not check for suspension, as we need to jump back to @c yield_target
				template<typename Promise>
				static
				auto await_suspend(std::coroutine_handle<Promise> self) noexcept { return self.promise().yield_target; }
				static
				void await_resume() noexcept {}
			};
		};
	private:
		//! @brief lazy iterator for elements yielded by a coroutine
		struct iterator final {
			using value_type = value;
			using difference_type = std::ptrdiff_t;

			iterator(iterator && other) noexcept : handle{std::exchange(other.handle, {})} {}
			auto operator=(iterator && other) noexcept -> iterator & {
				std::swap(handle, other.handle);
				return *this;
			}
			~iterator() noexcept { if(handle) handle.destroy(); }

			auto operator*() const -> reference /*TODO: [C++26] pre(handle and not handle.done())*/ { return static_cast<reference>(*handle.promise().ptr); }

			//! @returns awaiter for lazy increment
			//! @attention the returned awaiter must be awaited on on the coroutine that initially awaited @c generator::begin
			auto operator++() /*TODO: [C++26] pre(handle and not handle.done())*/ { return internal::promise_base::iterator_awaiter<iterator &, false>{*this}; }

			friend
			auto operator==(const iterator & self, std::default_sentinel_t) -> bool /*TODO: [C++26] pre(self.handle)*/ { return self.handle.done(); } //TODO: alternative to precondition: not self.handle == sentinel
		private:
			friend
			generator;
			friend
			auto internal::get_handle(auto &) noexcept;

			iterator(std::coroutine_handle<promise_type> handle) noexcept : handle{handle} {}

			std::coroutine_handle<promise_type> handle;
		};
	public:
		auto valueless() const noexcept -> bool { return not handle; }

		//! @returns awaiter for the initial iterator
		//! @attention transfers ownership of the managed coroutine to the resulting iterator
		//! @attention the returned iterator is bound to the calling coroutine
		auto begin() /*TODO: [C++26] pre(not valueless()) post(valueless())*/ { return internal::promise_base::iterator_awaiter<iterator, true>{std::exchange(handle, {})}; }
		static
		auto end() noexcept -> std::default_sentinel_t { return std::default_sentinel; }

		generator(generator && other) noexcept : handle{std::exchange(other.handle, {})} {}
		auto operator=(generator && other) noexcept -> generator & {
			std::swap(handle, other.handle);
			return *this;
		}
		~generator() noexcept { if(handle) handle.destroy(); }
	private:
		friend
		auto internal::get_handle(auto &) noexcept;

		generator(std::coroutine_handle<promise_type> handle) noexcept : handle{handle} {}

		std::coroutine_handle<promise_type> handle;
	};
}

