
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

//TODO: (especially) for generator: check all preconditions

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

		struct promise_base {
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
		};

		template<typename T>
		struct task_promise : promise_base {
			static_assert(std::is_object_v<T> and std::is_same_v<std::decay_t<T>, T>); //disqualify cv T &, T[], ...

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


		//! @brief internal accessor to handle
		auto get_handle(auto & val) noexcept { return val.handle; }


		template<typename Other>
		struct push_awaiter {
			Other other;
			promise_base::nested_info n;

			auto await_ready() const noexcept { return get_handle(other).done(); }

			template<typename Promise>
			auto await_suspend(std::coroutine_handle<Promise> self) noexcept -> std::coroutine_handle<> {
				get_handle(other).promise().nested = std::addressof(n);
				n.parent = self;
				auto nested{self.promise().nested};
				(n.root = nested ? nested->root : std::addressof(self.promise()))->top = get_handle(other);
				return n.root->must_suspend() ? std::noop_coroutine() : n.root->top;
			}

			auto await_resume() const /*TODO: [C++26] pre(other.handle.done())*/ { //TODO: remove precondition?
				if(n.eptr) std::rethrow_exception(n.eptr);
			}
		};


		template<typename Other>
		struct iterator_awaiter final {
			Other other;
			promise_base::nested_info n;
			std::coroutine_handle<> self;
			std::coroutine_handle<> * top;

			auto await_ready() const noexcept { return get_handle(other).done(); }
			//TODO: document this stuff in more detail
			template<typename Promise>
			auto await_suspend(std::coroutine_handle<Promise> self) noexcept -> std::coroutine_handle<> {
				auto other_handle{get_handle(other)};
				auto & other_promise{other_handle.promise()};

				//! @attention store @c self to restore as @c top on resumption
				this->self = self;
				//! @attention store address of @c root->top to be able to set it on resumption
				const auto & nested{self.promise().nested};
				top = std::addressof(nested ? nested->root->top : self.promise().top);
				//! @attention setup return target for generator's @c co_yield
				assert(not other_promise.yield_target or other_promise.yield_target == self);
				other_promise.yield_target = self;

				other_promise.nested = std::addressof(n);
				n.parent = self;
				(n.root = nested ? nested->root : std::addressof(self.promise()))->top = other_promise.top ? other_promise.top : other_handle;

				assert(n.root->top);
				return n.root->must_suspend() ? std::noop_coroutine() : n.root->top;
			}

			auto await_resume() const noexcept -> bool {
				auto other_handle{get_handle(other)};

				auto & promise{other_handle.promise()};
				promise.top = *top; //top of generator must point to logical top of stack
				promise.ptr = decltype(other_handle)::from_address(top->address()).promise().ptr; //pointer to result must be copied to direct promise as we can't navigate to "top" later
				promise.nested = nullptr; //we are no longer nested (TODO: redudant?)

				//! @attention restore @c root->top from before suspension
				*top = self;
				if(n.eptr) std::rethrow_exception(n.eptr);
				return not other_handle.done();
			}
		};
	}

	//! @brief tag to yield progress within a @c task
	inline
	constexpr
	internal::progress_t progress{1};

	//! @brief cooperative synchronous(!) recursive coroutine task
	//TODO: documentation for template parameter
	//! supported coroutine statements:
	//!  * @code{.cpp} co_yield progress; @endcode to yield control back from the coroutine to the caller
	//!  * @code{.cpp} [val =] co_await task; @endcode block this task until the awaited task is completed, optionally receiving a value
	//!  * @code{.cpp} co_return [val]; @endcode to terminate the task and optionally return a value to the caller
	//TODO: support for `for co_await`
	template<typename T> //TODO: allocator support, default argument for T? (better name for T?)
	struct task final {
		struct promise_type final : internal::task_promise<T> {
			promise_type() { this->top = std::coroutine_handle<promise_type>::from_promise(*this); }

			auto get_return_object() noexcept { return task{std::coroutine_handle<promise_type>::from_promise(*this)}; }

			template<typename U>
			static
			auto await_transform(task<U> && other) /*TODO: [C++26] pre(not other.valueless())*/ {
				struct awaiter : internal::push_awaiter<task<U>> {
					auto await_resume() const -> std::add_rvalue_reference_t<U> /*TODO: [C++26] pre(other.handle.done())*/ {
						internal::push_awaiter<task<U>>::await_resume();
						if constexpr(not std::is_void_v<U>) return std::move(*internal::get_handle(this->other).promise().result);
					}
				};
				return awaiter{std::move(other)};
			}

			template<typename... Us>
			static
			auto await_transform(internal::iterator_awaiter<Us...> other) { return other; }
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
			promise.suspend = nullptr; //TODO: redundant?
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
	//! @tparam Reference TODO
	//! @tparam Value TODO
	//TODO: document supported syntax
	template<typename Reference, typename Value = void>
	class generator final : public std::ranges::view_interface<generator<Reference, Value>> {
		using value = std::conditional_t<std::is_void_v<Value>, std::remove_cvref_t<Reference>, Value>;
		static_assert(std::is_object_v<value> && std::is_same_v<std::remove_cvref_t<value>, value>);

		using reference = std::conditional_t<std::is_void_v<Value>, Reference &&, Reference>;
		static_assert(std::is_reference_v<reference> || (std::is_object_v<reference> && std::is_same_v<std::remove_cv_t<reference>, reference> && std::copy_constructible<reference>));

		using rref = std::conditional_t<std::is_reference_v<reference>, std::remove_reference_t<reference> &&, reference>;
		static_assert(std::common_reference_with<reference &&, value &>);
		static_assert(std::common_reference_with<reference &&, rref &&>);
		static_assert(std::common_reference_with<rref &&, const value &>);
	public:
		using yielded = std::conditional_t<std::is_reference_v<reference>, reference, const reference &>;

		struct promise_type final : internal::promise_base {
			std::add_pointer_t<yielded> ptr;
			std::coroutine_handle<> yield_target;

			auto get_return_object() noexcept -> generator { return std::coroutine_handle<promise_type>::from_promise(*this); }

			using internal::promise_base::yield_value;

			auto yield_value(yielded val) noexcept {
				ptr = std::addressof(val);
				return yield_awaiter{};
			}

			auto yield_value(const std::remove_reference_t<yielded> & lval) requires std::is_rvalue_reference_v<yielded> && std::constructible_from<std::remove_cvref_t<yielded>, const std::remove_reference_t<yielded> &> {
				struct awaiter final : yield_awaiter {
					std::remove_cvref_t<yielded> val;

					auto await_suspend(std::coroutine_handle<promise_type> self) noexcept {
						self.promise().ptr = std::addressof(val);
						return yield_awaiter::await_suspend(self);
					}
				};
				return awaiter{{}, lval};
			}

			template<typename R2, typename V2>
			requires std::same_as<typename generator<R2, V2>::yielded, yielded>
			auto yield_value(ranges::elements_of<generator<R2, V2> &&> g) noexcept {
				g.range.handle.promise().yield_target = yield_target;
				return internal::push_awaiter<generator<R2, V2>>{std::move(g.range)};
			}

			template<std::ranges::input_range R>
			requires std::convertible_to<std::ranges::range_reference_t<R>, yielded>
			auto yield_value(ranges::elements_of<R> r) noexcept {
				auto wrapped{[](std::ranges::iterator_t<R> i, std::ranges::sentinel_t<R> s) -> generator<yielded, std::ranges::range_value_t<R>> { for (; i != s; ++i) co_yield static_cast<yielded>(*i); }};
				return yield_value(ranges::elements_of(wrapped(std::ranges::begin(r.range), std::ranges::end(r.range))));
			}

			//TODO: support for task??
			//TODO: support for `for co_wait`?

			void await_transform() =delete;

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
		struct iterator final {
			using value_type = value;
			using difference_type = std::ptrdiff_t;

			iterator(iterator && other) noexcept : handle{std::exchange(other.handle, {})} {}
			auto operator=(iterator && other) noexcept -> iterator & {
				handle = std::exchange(other.handle, {});
				return *this;
			}

			auto operator*() const noexcept(std::is_nothrow_copy_constructible_v<reference>) -> reference /*TODO: [C++26] pre(not handle.done())*/ { return static_cast<reference>(*handle.promise().ptr); }

			void operator++() /*TODO: [C++26] pre(not handle.done())*/ {}

			friend
			auto operator!=(const iterator & self, std::default_sentinel_t) { return internal::iterator_awaiter<const iterator &>{self}; }
		private:
			friend generator;
			friend
			auto internal::get_handle(auto &) noexcept;

			iterator(std::coroutine_handle<promise_type> handle) noexcept : handle{handle} {}

			std::coroutine_handle<promise_type> handle;
		};
	public:
		//TODO: valueless? (=> way to mark valueless on exception?)

		auto begin() -> iterator {
			//TODO: [C++??] precondition(handle­ refers to a coroutine suspended at its initial suspend point);
			return handle;
		}
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

		generator(std::coroutine_handle<promise_type> handle) : handle{std::move(handle)} {}

		std::coroutine_handle<promise_type> handle;
	};
}

