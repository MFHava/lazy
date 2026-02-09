
//          Copyright Michael Florian Hava.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file ../../LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#pragma once
#include <chrono>
#include <memory>
#include <cstdint>
#include <cstring>
#include <utility>
#include <coroutine>
#include <type_traits>

namespace lazy {
	template<typename>
	struct task;

	namespace internal {
		//! @brief internal accessor to handle
		auto get_handle(auto & val) noexcept { return val.handle; }

		struct resumption_t final {
			constexpr
			explicit
			resumption_t(int) noexcept {}
		};

		struct progress_t final {
			constexpr
			explicit
			progress_t(int) noexcept {}
		};

		struct active_root final {
			std::coroutine_handle<> top;

			//! @note inlined @c function_ref
			void * ctx;
			bool (*fptr)(void *) noexcept;

			auto suspend() const noexcept -> bool { return fptr(ctx); }
		};

		class promise_base {
			struct nested_info final {
				std::exception_ptr eptr;        //needed for manual stack unwinding
				std::coroutine_handle<> parent; //directly preceding coroutine
				promise_base * root;            //bottom of implicit coroutine-"stack" @attention due to possibility of resumption before nesting, this may not actually be the root and must instead be followed recursively (see @c find_root ) to find actual root
			};

			auto get_nested() const -> nested_info * { return (data & 1U) ? reinterpret_cast<nested_info *>(data ^ 1U) : nullptr; }
			void set_nested(nested_info & nested) /*TODO: [C++26] post(data & 1U)*/ { data = reinterpret_cast<std::uintptr_t>(&nested) | 1U; } 

			static
			auto find_root(const promise_base * start) -> promise_base * /*TODO: [C++26] pre(start) post(r: r)*/ {
				//! @attention due to manual resumption of tasks root may not actually point to root => follow chain until we find correct root
				for(auto ptr{const_cast<promise_base *>(start)};;) {
					if(auto nested{ptr->get_nested()}) ptr = nested->root;
					else return ptr;
				}
			}
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

			auto await_transform(internal::resumption_t) const noexcept {
				struct awaiter final {
					const bool suspend;

					auto await_ready() const noexcept { return not suspend; }
					static
					void await_suspend(std::coroutine_handle<>) noexcept {}
					static
					void await_resume() noexcept {}
				};

				//! @note determine suspension here to avoid redundant suspend-resume when inspecting handle in @c await_suspend ...
				auto ar{reinterpret_cast<active_root *>(find_root(this)->data)};
				return awaiter{ar->suspend()};
			}

			template<typename T>
			static
			auto await_transform(task<T> other) /*TODO: [C++26] pre(not other.valueless())*/ {
				struct awaiter : push_awaiter<task<T>> {
					auto await_resume() const -> std::add_rvalue_reference_t<T> /*TODO: [C++26] pre(other.done())*/ {
						push_awaiter<task<T>>::await_resume();
						return std::move(internal::get_handle(this->other).promise()).get_value();
					}
				};
				return awaiter{std::move(other)};
			}

			template<typename Other, bool Initial>
			class iterator_awaiter final {
				static_assert((Initial and not std::is_reference_v<Other>) or (not Initial and std::is_lvalue_reference_v<Other>));

				Other other;
				nested_info n;
				std::coroutine_handle<> prev_top;
			public:
				iterator_awaiter(Other other) requires(not Initial) /*TODO: [C++26] pre(get_handle(other) and not get_handle(other).done())*/ : other{other} {}
				iterator_awaiter(Other other) requires(Initial) /*TODO: [C++26] pre(get_handle(other) and not get_handle(other).done())*/ : other{std::move(other)} {}

				auto await_ready() const noexcept { return false; }

				template<typename Promise>
				auto await_suspend(std::coroutine_handle<Promise> self) noexcept -> std::coroutine_handle<> {
					auto & other_promise{get_handle(other).promise()};
					//TODO: [C++26] contract_assert(not (other_promise.data & 1U));

					//! @attention connect @c other 's @c co_yield with current coroutine frame
					if constexpr(Initial) other_promise.yield_target = self;
					//TODO: [C++26] else contract_assert(other_promise.yield_target == self);

					//! @attention store enough context to remove @c other from stack on resumption (as @c generator is not permanently on top of stack)
					prev_top = n.parent = self;

					//! @attention push @c other onto stack
					n.root = find_root(std::addressof(self.promise()));

					auto ar{reinterpret_cast<active_root *>(n.root->data)};
					ar->top = std::coroutine_handle<>::from_address(reinterpret_cast<void *>(other_promise.data));
					other_promise.set_nested(n);

					return ar->suspend() ? std::noop_coroutine() : ar->top;
				}

				auto await_resume() {
					//! @note must be checked first, because if we got here via an unhandled exception, there is nothing to do apart from rethrowing
					if(n.eptr) std::rethrow_exception(n.eptr);

					//! @attention task may have been nested after iteration was started
					auto ar{reinterpret_cast<active_root *>(find_root(n.root)->data)};
					//! @attention @c other_promise.top won't be up to date, need to get actual top from @c *top so we can resume the correct coroutine on the next iteration
					get_handle(other).promise().data = reinterpret_cast<std::uintptr_t>(ar->top.address());
					//! @attention pop @c other from stack by restoring the @c top we had on @c await_suspend
					ar->top = prev_top;

					if constexpr(Initial) return std::move(other);
				}
			};

			template<typename T, bool U>
			static
			auto await_transform(iterator_awaiter<T, U> other) { return other; }
		private:
			//memory layout:
			//     [ coroutine frame ] [ deleter ]   [ offset ] [ padding  ] [ allocator ]
			//     [       ? B       ] [   8 B   ]   [   1 B  ] [ offset B ] [    ? B    ]
			//     [--      always present     --]   [--   only for statefull alloc    --]

			using deleter_t = void(*)(void *, std::size_t) noexcept;
			static_assert(sizeof(deleter_t) == sizeof(std::uintptr_t));

			template<typename Alloc>
			static
			auto allocate(std::size_t size, const Alloc & alloc, const auto &...) -> void * {
				using A = std::allocator_traits<Alloc>::template rebind_alloc<std::byte>;

				constexpr auto stateless_allocator{std::is_default_constructible_v<A> and std::allocator_traits<A>::is_always_equal::value};

				const auto offset{stateless_allocator ? 0 : ((alignof(A) - (size + sizeof(deleter_t) + 1) % alignof(A)) % alignof(A))};
				//TODO: [C++26] contract_assert(offset <= 255);
				const auto capacity{size + sizeof(deleter_t) + (stateless_allocator ? 0 : 1 + offset + sizeof(A))};

				auto dealloc{+[](std::byte * ptr, std::size_t size) noexcept {
					if constexpr(stateless_allocator) A{}.deallocate(ptr, size + sizeof(deleter_t));
					else {
						const auto offset{static_cast<std::size_t>(*(ptr + size + sizeof(deleter_t)))};
						auto pa{reinterpret_cast<A *>(ptr + size + sizeof(deleter_t) + 1 + offset)};
						A alloc{std::move(*pa)};
						pa->~A();
						alloc.deallocate(ptr, size + sizeof(deleter_t) + 1 + offset + sizeof(A));
					}
				}};

				//! @note allocators may not throw on construction, destruction, nor rebind but may not be marked as @c noexcept
				A a{alloc};
				auto ptr{a.allocate(capacity)};
				auto d{reinterpret_cast<std::uintptr_t>(dealloc)};
				std::memcpy(ptr + size, &d, sizeof(d));
				if constexpr(not stateless_allocator) {
					*(ptr + size + sizeof(deleter_t)) = static_cast<std::byte>(offset);
					new(ptr + size + sizeof(deleter_t) + 1 + offset) A{std::move(a)};
				}
				return ptr;
			}
		public:
			//! @note no allocator
			static
			auto operator new(std::size_t size) -> void * { return allocate(size, std::allocator<char>{}); }

			//! @note allocator, non-member function
			static
			auto operator new(std::size_t size, std::allocator_arg_t, const auto &... args) -> void * {
				static_assert(sizeof...(args), "if allocator_arg_t is first argument, the second argument must be an allocator");
				return allocate(size, args...);
			}

			//! @note allocator, member function
			static
			auto operator new(std::size_t size, const auto &, std::allocator_arg_t, const auto &... args) -> void * {
				static_assert(sizeof...(args), "if allocator_arg_t is first argument, the second argument must be an allocator");
				return allocate(size, args...);
			}

			//! @note must handle all versions of @code{.cpp} operator new() @encode
			static
			void operator delete(void * ptr, std::size_t size) noexcept {
				std::uintptr_t d;
				std::memcpy(&d, static_cast<char *>(ptr) + size, sizeof(std::uintptr_t));
				//TODO: [C++26] contract_assert(d);
				reinterpret_cast<deleter_t>(d)(static_cast<std::byte *>(ptr), size);
			}
		private:
			struct pop_awaiter final {
				static
				auto await_ready() noexcept { return false; }

				template<typename Promise>
				static
				auto await_suspend(std::coroutine_handle<Promise> self) noexcept -> std::coroutine_handle<> {
					if(const auto nested{self.promise().get_nested()}) {
						auto ar{reinterpret_cast<active_root *>(find_root(nested->root)->data)};
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
			class push_awaiter {
				nested_info n;
			protected:
				Other other;
			public:
				push_awaiter(Other other) /*TODO: [C++26] pre(get_handle(other))*/ : other{std::move(other)} {}

				auto await_ready() const noexcept { return get_handle(other).done(); }

				template<typename Promise>
				auto await_suspend(std::coroutine_handle<Promise> self) noexcept -> std::coroutine_handle<> {
					auto & promise{get_handle(other).promise()};
					//TODO: [C++26] contract_assert(not promise.get_nested());
					auto top{std::coroutine_handle<>::from_address(reinterpret_cast<void *>(promise.data))};
					promise.set_nested(n);
					n.parent = self;
					n.root = find_root(std::addressof(self.promise()));
					auto ar{reinterpret_cast<active_root *>(n.root->data)};
					ar->top = top;
					return ar->suspend() ? std::noop_coroutine() : ar->top;
				}

				auto await_resume() const /*TODO: [C++26] pre(get_handle(other).done())*/ {
					if(n.eptr) std::rethrow_exception(n.eptr);
				}
			};
		};

		template<typename T>
		class task_promise : public promise_base {
			union { T result; };
			bool initialized{false};
		public:
			task_promise() noexcept {}
			task_promise(const task_promise &) =delete;
			auto operator=(const task_promise &) -> task_promise & =delete;
			~task_promise() noexcept { if(initialized) result.~T(); }

			template<typename U = T>
			void return_value(U && value) /*TODO: [C++26] pre(not initialized)*/ {
				new(&result) T(std::forward<U>(value));
				initialized = true;
			}

			auto get_value() & -> T & /*TODO: [C++26] pre(initialized)*/ { return result; }
			auto get_value() && -> T && /*TODO: [C++26] pre(initialized)*/ { return std::move(result); }
		};

		template<>
		struct task_promise<void> : promise_base {
			static
			void return_void() noexcept {}

			static
			void get_value() noexcept {}
		};
	}

	//! @brief tag to yield progress within a @c task or @c generator
	inline
	constexpr
	internal::resumption_t resumption{1};


	//! @brief tag to yield progress within a @c task
	//! @note not supported in @c generator to avoid ambiguity problems
	inline
	constexpr
	internal::progress_t progress{1};


	//! @brief cooperative synchronous(!) recursive coroutine task
	//! @tparam Result return type of the task
	//! supported coroutine statements:
	//!  * @code{.cpp} co_yield progress; @endcode to yield control back from the coroutine to the caller
	//!  * @code{.cpp} co_await resumption; @endcode to yield control back from the coroutine to the caller
	//!  * @code{.cpp} [val =] co_await task; @endcode block this task until the awaited @c task is completed, optionally receiving a value
	//!  * @code{.cpp} for co_await(<type> val : gen) { ... } @endcode block this task until awaited generator yields next value
	//!  * @code{.cpp} co_return [val]; @endcode to terminate the task and optionally return a value to the caller
	template<typename Result = void>
	struct task final {
		static_assert(std::is_void_v<Result> or (std::is_object_v<Result> and std::is_same_v<std::decay_t<Result>, Result>));

		struct promise_type final : internal::task_promise<Result> {
			promise_type() { this->data = reinterpret_cast<std::uintptr_t>(std::coroutine_handle<promise_type>::from_promise(*this).address()); }

			auto get_return_object() noexcept { return task{std::coroutine_handle<promise_type>::from_promise(*this)}; }

			auto yield_value(internal::progress_t) const noexcept { return this->await_transform(resumption); }
		};

		auto valueless() const noexcept -> bool { return not handle; }

		auto done() const -> bool /*TODO: [C++26] pre(not valueless())*/ { return handle.done(); }

		void wait() /*TODO: [C++26] pre(not valueless()) post(done())*/ { wait_with([]() noexcept { return false; }); }

		template<typename Rep, typename Period>
		auto wait_for(const std::chrono::duration<Rep, Period> & duration) -> bool /*TODO: [C++26] pre(not valueless())*/ { return wait_until(std::chrono::steady_clock::now() + duration); }

		template<typename Clock, typename Duration>
		auto wait_until(const std::chrono::time_point<Clock, Duration> & time) -> bool /*TODO: [C++26] pre(not valueless())*/ {
#if __cpp_lib_chrono >= 201907L
			static_assert(std::chrono::is_clock_v<Clock>);
#endif
			return wait_with([&]() noexcept { return Clock::now() >= time; });
		}

		template<typename Func>
		auto wait_with(Func func) -> bool /*TODO: [C++26] pre(not valueless())*/ {
			static_assert(requires { { func() } noexcept -> std::same_as<bool>; });
			if(done()) return true;
			try {
				auto & promise{handle.promise()};
				internal::active_root ar{
					.top = std::coroutine_handle<>::from_address(reinterpret_cast<void *>(promise.data)),
					.ctx = std::addressof(func),
					.fptr = +[](void * ptr) noexcept { return (*reinterpret_cast<Func *>(ptr))(); }
				};
				//TODO: [C++26] contract_assert(ar.top and not ar.top.done());
				promise.data = reinterpret_cast<std::uintptr_t>(std::addressof(ar));
				ar.top.resume();
				promise.data = reinterpret_cast<std::uintptr_t>(ar.top.address());
				return done();
			} catch(...) {
				std::exchange(handle, {}).destroy(); //! @attention mark @c *this as @c valueless to trigger precondition violations on future usage
				throw;
			}
		}

		auto get() -> std::add_lvalue_reference_t<Result> /*TODO: [C++26] pre(not valueless()) post(done())*/ {
			wait();
			return handle.promise().get_value();
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

		std::coroutine_handle<promise_type> handle;
	};


	//! @brief cooperative synchronous(!) recursive coroutine generator
	//! @tparam Reference reference type of generator
	//! @tparam Value value type of the generator
	//! supported coroutine statements:
	//!  * @code{.cpp} co_await resumption; @endcode to yield control back from the coroutine to the caller
	//!  * @code{.cpp} [val =] co_await task; @endcode block this generator until the awaited @c task is completed, optionally receiving a value
	//!  * @code{.cpp} for co_await(<type> val : gen) { ... } @endcode block this generator until awaited generator yields next value
	//!  * @code{.cpp} co_await generator; @endcode yield elements of @c generator
	//!  * @code{.cpp} co_yield val; @endcode yield value to caller of generator
	template<typename Reference, typename Value = void>
	class generator final {
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

			auto yield_value(yielded val) /*TODO: [C++26] pre(yield_target and not yield_target.done())*/ {
				ptr = std::addressof(val);
				return yield_awaiter{};
			}

			auto yield_value(const std::remove_reference_t<yielded> & lval) requires std::is_rvalue_reference_v<yielded> and std::constructible_from<std::remove_cvref_t<yielded>, const std::remove_reference_t<yielded> &> /*TODO: [C++26] pre(yield_target and not yield_target.done())*/ {
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

			auto await_transform(generator other) /*TODO: [C++26] pre(not other.valueless() and not other.handle.promise().yield_target) pre(yield_target and not yield_target.done())*/ {
				other.handle.promise().yield_target = yield_target;
				return internal::promise_base::push_awaiter{std::move(other)};
			}

			static
			void return_void() noexcept {}
		private:
			struct yield_awaiter {
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

			auto operator*() const -> reference /*TODO: [C++26] pre(handle and not handle.done())*/ {
				auto & promise{handle.promise()};
				//TODO: [C++26] contract_assert(not (promise.data & 1U));
				auto top{std::coroutine_handle<promise_type>::from_address(reinterpret_cast<void *>(promise.data))};
				return static_cast<reference>(*top.promise().ptr);
			}

			//! @returns awaiter for lazy increment
			//! @attention the returned awaiter must be awaited on on the coroutine that initially awaited @c generator::begin
			auto operator++() /*TODO: [C++26] pre(handle and not handle.done())*/ { return internal::promise_base::iterator_awaiter<iterator &, false>{*this}; }

			friend
			auto operator==(const iterator & self, std::default_sentinel_t) -> bool /*TODO: [C++26] pre(self.handle)*/ { return self.handle.done(); }
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

