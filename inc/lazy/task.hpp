
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

		template<typename T>
		struct iterator final { const T & it; };

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


			static
			auto initial_suspend() noexcept -> std::suspend_always { return {}; }

			void unhandled_exception() const {
				if(this->nested) this->nested->eptr = std::current_exception();
				else throw;
			}
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


		struct pop_awaiter final {
			static
			auto await_ready() noexcept { return false; }

			template<typename Promise>
			static
			auto await_suspend(std::coroutine_handle<Promise> self) noexcept -> std::coroutine_handle<> {
				if(const auto nested{self.promise().nested}) {
					nested->root->top = nested->parent;
					/*TODO: if(not nested->root->must_suspend())*/ return nested->root->top;
				}
				return std::noop_coroutine();
			}

			static
			void await_resume() noexcept {}
		};

		template<typename Other>
		struct push_awaiter {
			Other other;
			nested_info n;

			auto await_ready() const noexcept { return other.handle.done(); }

			template<typename Promise>
			auto await_suspend(std::coroutine_handle<Promise> self) noexcept -> std::coroutine_handle<> {
				other.handle.promise().nested = std::addressof(n);
				n.parent = self;
				auto nested{self.promise().nested};
				(n.root = nested ? nested->root : std::addressof(self.promise()))->top = other.handle;
				return /*TODO: n.root->must_suspend() ? std::noop_coroutine() :*/ n.root->top;
			}

			auto await_resume() const /*TODO: [C++26] pre(other.handle.done())*/ { //TODO: remove precondition?
				if(n.eptr) std::rethrow_exception(n.eptr);
			}
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
			auto final_suspend() noexcept -> internal::pop_awaiter { return {}; }

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

			template<typename U>
			static
			auto await_transform(task<U> && other) /*TODO: [C++26] pre(not other.valueless())*/ {
				struct awaiter : internal::push_awaiter<task<U>> {
					auto await_resume() const -> std::add_rvalue_reference_t<U> /*TODO: [C++26] pre(other.handle.done())*/ {
						internal::push_awaiter<task<U>>::await_resume();
						if constexpr(not std::is_void_v<U>) return std::move(*this->other.handle.promise().result);
					}
				};
				return awaiter{std::move(other)};
			}

			template<typename U>
			static
			auto await_transform(internal::iterator<U> other) {
				struct awaiter final {
					const U & it;
					std::coroutine_handle<promise_type> self;

					auto await_ready() const noexcept -> bool { return it.handle.done(); }
					auto await_suspend(std::coroutine_handle<promise_type> self) noexcept -> std::coroutine_handle<> {
						this->self = self;
						it.handle.promise().parent_task = self;
						it.handle.promise().suspend = self.promise().suspend;
						if(self.promise().nested) self.promise().nested->root->top = it.handle.promise().top;
						else self.promise().top = it.handle.promise().top;
						return it.handle.promise().top;
					}
					auto await_resume() const noexcept -> bool {
						it.handle.promise().parent_task = std::coroutine_handle<>{};
						it.handle.promise().suspend = nullptr;
						if(self.promise().nested) self.promise().nested->root->top = self;
						else self.promise().top = self;
						//TODO: exception?
						return not it.handle.done();
					}
				};

				return awaiter{other.it};
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

	public: //TODO: remove
		std::coroutine_handle<promise_type> handle;
	};


//TODO: still sync generator

	namespace ranges {
		template</*TODO std::ranges::range*/ typename Range>
		struct elements_of {
			Range range;
		};

		template<typename Range>
		elements_of(Range &&) -> elements_of<Range &&>; //TODO: necessary?!
	}

	//! @brief lazy view of elements yielded by a coroutine
	//! @tparam Reference TODO
	//! @tparam Value TODO
	template<typename Reference, typename Value = void>
	class generator final : public std::ranges::view_interface<generator<Reference, Value>> {
		using value = std::conditional_t<std::is_void_v<Value>, std::remove_cvref_t<Reference>, Value>; //exposition only
		static_assert(std::is_object_v<value> && std::is_same_v<std::remove_cvref_t<value>, value>);

		using reference = std::conditional_t<std::is_void_v<Value>, Reference &&, Reference>; //exposition only
		static_assert(std::is_reference_v<reference> || (std::is_object_v<reference> && std::is_same_v<std::remove_cv_t<reference>, reference> && std::copy_constructible<reference>));

		using rref = std::conditional_t<std::is_reference_v<reference>, std::remove_reference_t<reference> &&, reference>;
		static_assert(std::common_reference_with<reference &&, value &>);
		static_assert(std::common_reference_with<reference &&, rref &&>);
		static_assert(std::common_reference_with<rref &&, const value &>);

		struct iterator;
	public:
		using yielded = std::conditional_t<std::is_reference_v<reference>, reference, const reference &>;

		class promise_type final {
			friend iterator;

			struct nested_info final {
				std::exception_ptr eptr;
				std::coroutine_handle<promise_type> bottom, parent; //"stack" navigation
			} * nested{nullptr};

			std::add_pointer_t<yielded> ptr{nullptr};
		public: //TODO: remove
			std::coroutine_handle<promise_type> top{std::coroutine_handle<promise_type>::from_promise(*this)};
			std::coroutine_handle<> parent_task{std::noop_coroutine()};
			internal::function_ref * suspend{nullptr};
		public:
			auto get_return_object() noexcept -> generator { return std::coroutine_handle<promise_type>::from_promise(*this); }

			auto initial_suspend() const noexcept -> std::suspend_always { return {}; }
			auto final_suspend() noexcept {
				struct awaitable final {
					auto await_ready() const noexcept -> bool { return false; }
					auto await_suspend(std::coroutine_handle<promise_type> handle) noexcept -> std::coroutine_handle<> {
						if(auto nested{handle.promise().nested}) {
							auto parent{nested->parent};
							nested->bottom.promise().top = parent;
							parent.promise().top = parent;
							return parent;
						} else return handle.promise().parent_task;
					}
					void await_resume() const noexcept {}
				};
				return awaitable{};
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

			auto yield_value(yielded val) noexcept {
				ptr = std::addressof(val);
				struct awaiter final {
					static
					auto await_ready() noexcept { return false; }
					static
					auto await_suspend(std::coroutine_handle<promise_type> self) noexcept {
						if(auto nested{self.promise().nested}) return nested->bottom.promise().parent_task;
						else return self.promise().parent_task;
					}
					static
					void await_resume() noexcept {}
				};
				return awaiter{};
			}

			auto yield_value(const std::remove_reference_t<yielded> & lval) requires std::is_rvalue_reference_v<yielded> && std::constructible_from<std::remove_cvref_t<yielded>, const std::remove_reference_t<yielded> &> {
				struct awaitable final {
					std::remove_cvref_t<yielded> val;

					static
					auto await_ready() noexcept -> bool { return false; }
					auto await_suspend(std::coroutine_handle<promise_type> self) noexcept {
						self.promise().ptr = std::addressof(val);
						if(auto nested{self.promise().nested}) return nested->bottom.promise().parent_task;
						else return self.promise().parent_task;
					}
					static
					void await_resume() noexcept {}
				};
				return awaitable{lval};
			}

			template<typename R2, typename V2>
			requires std::same_as<typename generator<R2, V2>::yielded, yielded>
			auto yield_value(ranges::elements_of<generator<R2, V2> &&> g) noexcept {
				struct awaitable final {
					generator<R2, V2> g;
					nested_info n;

					auto await_ready() const noexcept -> bool { return false; }
					auto await_suspend(std::coroutine_handle<promise_type> handle) {
						g.handle.promise().nested = &n;
						n.parent = handle;
						auto & parent_promise{handle.promise()};
						(n.bottom = parent_promise.nested ? parent_promise.nested->bottom : (assert(parent_promise.top == handle), parent_promise.top)).promise().top = g.handle; //TODO: remove assert...
						return g.handle;
					}
					void await_resume() const { if(n.eptr) std::rethrow_exception(n.eptr); }
				};
				return awaitable{std::move(g.range)};
			}

			template<std::ranges::input_range R>
			requires std::convertible_to<std::ranges::range_reference_t<R>, yielded>
			auto yield_value(ranges::elements_of<R> r) noexcept {
				auto wrapped{[](std::ranges::iterator_t<R> i, std::ranges::sentinel_t<R> s) -> generator<yielded, std::ranges::range_value_t<R>> { for (; i != s; ++i) co_yield static_cast<yielded>(*i); }};
				return yield_value(ranges::elements_of(wrapped(std::ranges::begin(r.range), std::ranges::end(r.range))));
			}

			void await_transform() =delete;

			void return_void() const noexcept {}
			void unhandled_exception() {
				if(nested) nested->eptr = std::current_exception();
				else throw;
			}
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

			auto operator*() const noexcept(std::is_nothrow_copy_constructible_v<reference>) -> reference {
				//TODO: [C++??] precondition(!handle.done());
				return static_cast<reference>(*handle.promise().top.promise().ptr);
			}

			auto operator++() -> iterator & {
				//TODO: [C++??] precondition(!handle.done());
				return *this;
			}
			void operator++(int) { ++*this; }

			friend
			auto operator!=(const iterator & i, std::default_sentinel_t) { return internal::iterator{i}; }
		private:
			friend generator;
			iterator(std::coroutine_handle<promise_type> handle) noexcept : handle{handle} {}
		public: //TODO: remove
			std::coroutine_handle<promise_type> handle;
		};
	public:
		generator(const generator &) =delete;
		generator(generator && other) noexcept : handle{std::exchange(other.handle, {})} {}

		auto operator=(generator other) noexcept -> generator & {
			std::swap(handle, other.handle);
			return *this;
		}

		~generator() noexcept { if(handle) handle.destroy(); }

		auto begin() -> iterator {
			//TODO: [C++??] precondition(handle­ refers to a coroutine suspended at its initial suspend point);
			return handle;
		}
		auto end() const noexcept -> std::default_sentinel_t { return std::default_sentinel; }
	private:
		friend promise_type;
		generator(std::coroutine_handle<promise_type> handle) : handle{std::move(handle)} {}

		std::coroutine_handle<promise_type> handle{nullptr}; //exposition only
	};
}

