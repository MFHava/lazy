
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

		struct function_ref final {
			const void * ctx;
			bool (*fptr)(const void *) noexcept;
		};

		struct promise_base {
			//TODO: top and nested should be mutually exclusive
			std::coroutine_handle<> top; //top of implicit stack
			struct nested_info final {
				std::exception_ptr eptr;        //needed for manual stack unwinding
				std::coroutine_handle<> parent; //directly preceding coroutine
				promise_base * root;            //bottom of implicit coroutine-"stack"
			} * nested{nullptr};
			function_ref * suspend{nullptr}; //callback to check for suspension on co_yield  - nullptr => never suspend

			auto must_suspend() const -> bool /*TODO: [C++26] pre(not nested)*/ { return suspend ? suspend->fptr(suspend->ctx) : false; }


			static
			auto initial_suspend() noexcept -> std::suspend_always { return {}; }
			static
			auto final_suspend() noexcept { return pop_awaiter{}; }

			void unhandled_exception() const {
				if(this->nested) this->nested->eptr = std::current_exception();
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
				return awaiter{suspend ? suspend->fptr(suspend->ctx) : false};
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
			struct iterator_awaiter final { //TODO: add contracts
				Other other;
				nested_info n;
				std::coroutine_handle<> prev_top;
				std::coroutine_handle<> * top_of_root;

				auto await_ready() const noexcept { return get_handle(other).done(); }

				template<typename Promise>
				auto await_suspend(std::coroutine_handle<Promise> self) noexcept -> std::coroutine_handle<> {
					auto other_handle{get_handle(other)};
					auto & other_promise{other_handle.promise()};
					const auto & nested{self.promise().nested};

					//! @attention connect @c other 's @c co_yield with current coroutine frame
					//! @note @c yield_target will never be reset, a once-started @c generator cannot be transfered to a different coroutine
					if constexpr(Initial) other_promise.yield_target = self;
					//TODO: [C++26] else contract_assert(other_promise.yield_target == self);

					//! @attention store enough context (@c top and @c self ) to remove @c other from stack on resumption (as @c generator is not permanently on top of stack)
					prev_top = self;
					top_of_root = std::addressof(nested ? nested->root->top : self.promise().top);

					//! @attention push @c other (which contrary to normal push could already be nested ...) onto stack
					n.parent = self;
					(n.root = nested ? nested->root : std::addressof(self.promise()))->top = other_promise.top;
					other_promise.nested = std::addressof(n);

					return n.root->must_suspend() ? std::noop_coroutine() : n.root->top;
				}

				auto await_resume() {
					//! @note must be checked first, because if we got here via an unhandled exception, there is nothing to do apart from rethrowing
					if(n.eptr) std::rethrow_exception(n.eptr);

					auto other_handle{get_handle(other)};
					auto & other_promise{other_handle.promise()};

					//! @attention @c other_promise.top won't be up to date, need to get actual top from @c *top so we can resume the correct coroutine on the next iteration
					other_promise.top = *top_of_root;

					//! @attention due to type-erasure we can't get the correct @c ptr from @c top => copy said pointer to the "root" (only valid if resumption was due to yield)
					if(not other_handle.done()) other_promise.ptr = decltype(other_handle)::from_address(top_of_root->address()).promise().ptr;

					//! @attention pop @c other from stack by restoring the @c top we had on @c await_suspend
					*top_of_root = prev_top;

					if constexpr(Initial) return std::move(other);
				}
			};

			template<typename T, bool U>
			static
			auto await_transform(iterator_awaiter<T, U> other) { return other; }
		private:
			struct pop_awaiter final {
				static
				auto await_ready() noexcept { return false; }

				template<typename Promise>
				static
				auto await_suspend(std::coroutine_handle<Promise> self) noexcept -> std::coroutine_handle<> {
					if(const auto nested{self.promise().nested}) {
						nested->root->top = nested->parent;
						if(not nested->root->must_suspend()) return nested->root->top;
					}
					return std::noop_coroutine();
				}

				static
				void await_resume() noexcept {}
			};
		protected:
			template<typename Other>
			struct push_awaiter {
				Other other;
				nested_info n;

				auto await_ready() const noexcept { return get_handle(other).done(); }

				template<typename Promise>
				auto await_suspend(std::coroutine_handle<Promise> self) noexcept -> std::coroutine_handle<> {
					get_handle(other).promise().nested = std::addressof(n);
					n.parent = self;
					auto nested{self.promise().nested};
					(n.root = nested ? nested->root : std::addressof(self.promise()))->top = get_handle(other);
					return n.root->must_suspend() ? std::noop_coroutine() : n.root->top;
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
	//!  * @code{.cpp} [val =] co_await task; @endcode block this task until the awaited task is completed, optionally receiving a value
	//!  * @code{.cpp} for co_await(<type> val : gen) { ... } @endcode block this task until awaited generator yields next value
	//!  * @code{.cpp} co_return [val]; @endcode to terminate the task and optionally return a value to the caller
	template<typename Result = void>
	struct task final {
		static_assert(std::is_void_v<Result> or (std::is_object_v<Result> and std::is_same_v<std::decay_t<Result>, Result>));

		struct promise_type final : internal::task_promise<Result> {
			promise_type() { this->top = std::coroutine_handle<promise_type>::from_promise(*this); }

			auto get_return_object() noexcept { return task{std::coroutine_handle<promise_type>::from_promise(*this)}; }
		};

		auto valueless() const noexcept -> bool { return not handle; }

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
			internal::function_ref f{
				.ctx = std::addressof(time),
				.fptr = +[](const void * ptr) noexcept { return Clock::now() >= *reinterpret_cast<Time *>(ptr); }
			};
			promise.suspend = &f;
			resume(promise.top);
			promise.suspend = nullptr;
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
		friend promise_type;
		friend
		auto internal::get_handle(auto &) noexcept;

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


	namespace ranges {
		template</*TODO std::ranges::range*/ typename Range>
		struct elements_of {
			Range range;
		};

		template<typename Range>
		elements_of(Range &&) -> elements_of<Range &&>;
	}

	//! @brief lazy view of elements yielded by a coroutine
	//! @tparam Reference reference type of generator
	//! @tparam Value value type of the generator
	//! supported coroutine statements:
	//!  * @code{.cpp} co_yield progress; @endcode to yield control back from the coroutine to the caller
	//!  * @code{.cpp} co_yield val; @endcode yield value to caller of generator
	//!  * @code{.cpp} [val =] co_await task; @endcode block this generator until the awaited task is completed, optionally receiving a value
	//!  * @code{.cpp} for co_await(<type> val : gen) { ... } @endcode block this generatro until awaited generator yields next value
	//TODO: support for `co_yield ranges::elements_of{g};`
	template<typename Reference, typename Value = void>
	class generator final : public std::ranges::view_interface<generator<Reference, Value>> {
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

			promise_type() { this->top = std::coroutine_handle<promise_type>::from_promise(*this); }

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

			template<typename R, typename V>
			requires std::same_as<typename generator<R, V>::yielded, yielded>
			auto yield_value(ranges::elements_of<generator<R, V> &&> g) noexcept { //TODO: remove need for ranges::elements_of??
				g.range.handle.promise().yield_target = yield_target;
				return internal::promise_base::push_awaiter<generator<R, V>>{std::move(g.range)};
			}

			template<std::ranges::input_range R>
			requires std::convertible_to<std::ranges::range_reference_t<R>, yielded> //TODO: remove?? (or use std::ranges::elements_of) ??
			auto yield_value(ranges::elements_of<R> r) noexcept {
				auto wrapped{[](std::ranges::iterator_t<R> i, std::ranges::sentinel_t<R> s) -> generator<yielded, std::ranges::range_value_t<R>> { for (; i != s; ++i) co_yield static_cast<yielded>(*i); }};
				return yield_value(ranges::elements_of(wrapped(std::ranges::begin(r.range), std::ranges::end(r.range))));
			}

			void return_void() const noexcept {}
		private:
			struct yield_awaiter {
				static
				auto await_ready() noexcept { return false; }
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
			auto operator++() /*TODO: [C++26] pre(handle and not handle.done())*/ { return internal::promise_base::iterator_awaiter<iterator &, false>{*this}; }

			friend
			auto operator==(const iterator & self, std::default_sentinel_t) -> bool /*TODO: [C++26] pre(self.handle)*/ { return self.handle.done(); } //TODO: alternative to precondition: not self.handle == sentinel
		private:
			friend generator;
			friend
			auto internal::get_handle(auto &) noexcept;

			iterator(std::coroutine_handle<promise_type> handle) noexcept : handle{handle} {}

			std::coroutine_handle<promise_type> handle;
		};
	public:
		auto valueless() const noexcept -> bool { return not handle; }

		//! @returns awaiter for the initial iterator
		//! @attention transfers ownership of the managed coroutine to the resulting iterator
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
		friend promise_type;
		friend
		auto internal::get_handle(auto &) noexcept;

		generator(std::coroutine_handle<promise_type> handle) noexcept : handle{handle} {}

		std::coroutine_handle<promise_type> handle;
	};
}

