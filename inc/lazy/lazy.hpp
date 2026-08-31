
//          Copyright Michael Florian Hava.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file ../../LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#pragma once
#include <span>
#include <atomic>
#include <chrono>
#include <format>
#include <memory>
#include <ranges>
#include <vector>
#include <cstdint>
#include <cstring>
#include <utility>
#include <variant>
#include <concepts>
#include <optional>
#include <coroutine>
#include <functional>
#include <type_traits>
#include <system_error>
#include <source_location>

//TODO: for all atomic operations: determine correct memory_order!
//TODO: add more documentation

//! @brief coroutine statements supported by all coroutine wrappers:
//!  * @code{.cpp} co_yield progress; @endcode to yield control back from the coroutine to the caller
//!  * @code{.cpp} co_yield blocked; @endcode to yield control back from the coroutine to the caller and signal that progress is not possible due to a synchronization primitive
//!  * @code{.cpp} co_await task; @endcode block current coroutine until the awaited @c task is completed, then returns its result if any
//!  * @code{.cpp} co_await [error|warning|info|debug]{fmt-string, args...}; @endcode create log of the respective severity
//!  * @code{.cpp} co_await <dump>; @endcode where @c <dump> is derived from @c dump_base create dump entry if coroutine stack is executing with @c log_level::trace
//!  * @code{.cpp} co_await get_identity; @endcode yields unique identification of coroutine stack
//!  * @code{.cpp} co_await get_is_tracing; @endcode yields @c true if coroutine stack is executing with @c log_level::trace
//!  * @code{.cpp} co_await timed{task}; @endcode block this coroutine until the awaited @c task is completed, then returns the time it took to complete and its result if any
//!  * @code{.cpp} for co_await(<type> val : gen) { ... } @endcode block this task until awaited generator yields next value
//! @attention as of C++26 @code{.cpp} for co_await @endcode is not supported and must be expanded manually to implemented as @code{.cpp} for(auto it{co_await gen.begin()}; it != gen.end(); co_await ++it) ... @endcode
namespace lazy {}

#ifndef __cpp_contracts
	#warning Contracts are not supported by your implementation, deactivating all contracts checks.

	#define pre(...)
	#define post(...)
	#define contract_assert(...) do {} while(0)
#endif

//! @brief compatibility shims for C++26 features / features not supported on every implementation
//! @attention only a subset of the actual feature is provided
namespace lazy::compat {
#if __cpp_lib_chrono < 201907L
	#warning std::chrono::is_clock_v is not supported by your implementation, using compat::is_clock_v as transitional solution.
	//! @note @c high_resolution_clock can be an alias, so dedicated specilizations are not an option...
	template<typename T>
	requires(std::same_as<T, std::chrono::steady_clock> or std::same_as<T, std::chrono::system_clock> or std::same_as<T, std::chrono::high_resolution_clock>)
	inline
	constexpr
	bool is_clock_v{true};
#else
	template<typename T>
	inline
	constexpr
	bool is_clock_v{std::chrono::is_clock_v<T>};
#endif

#if __cpp_lib_optional < 202506L
	#warning std::optional<T &> is not supported by your implementation, using compat::optional_ref as transitional solution.

	template<typename T>
	class optional_ref final { //TODO: [C++26] replace with std::optional<T &>
		T * ptr{nullptr};
	public:
		using value_type = T;

		optional_ref() noexcept =default;
		optional_ref(std::nullopt_t) noexcept {}

		optional_ref(T & val) noexcept : ptr{std::addressof(val)} {}

		void swap(optional_ref & other) noexcept { std::swap(ptr, other.ptr); }

		auto operator->() const -> T * pre(ptr) { return ptr; }
		auto operator*() const -> T & pre(ptr) { return *ptr; }

		explicit
		operator bool() const noexcept { return has_value(); }
		auto has_value() const noexcept -> bool { return ptr != nullptr; }

		auto value() const -> T & { return has_value() ? **this : throw std::bad_optional_access{}; }

		void reset() noexcept { ptr = nullptr; }

		template<typename U>
		friend
		auto operator<=>(optional_ref lhs, optional_ref<U> rhs) noexcept {
			if(lhs and rhs) return *lhs <=> *rhs;
			return lhs.has_value() <=> rhs.has_value();
		}
		template<typename U>
		friend
		auto operator==(optional_ref lhs, optional_ref<U> rhs) noexcept -> bool {
			if(lhs and rhs) return *lhs == *rhs;
			if(not lhs and not rhs) return true;
			return false;
		}

		template<typename U>
		friend
		auto operator<=>(optional_ref lhs, const U & rhs) noexcept { return lhs <=> optional_ref<const U>{rhs}; }
		template<typename U>
		friend
		auto operator==(optional_ref lhs, const U & rhs) noexcept -> bool { return lhs == optional_ref<const U>{rhs}; }
	};
#else
	template<typename T>
	using optional_ref = std::optional<T &>;
#endif

#ifndef __cpp_lib_function_ref
	#warning std::function_ref is not supported by your implementation, using compat::function_ref as transitional solution.

	template<typename... Signature>
	class function_ref; //TODO: [C++26] replace with std::function_ref

	template<typename Result, typename... Args>
	class function_ref<Result(Args...)> final {
		template<typename T>
		static
		constexpr
		bool is_invocable_using{std::is_invocable_r_v<Result, T, Args...>};

		void * self;
		Result(*disp)(void *, Args...);
	public:
		template<typename F, typename T = std::remove_reference_t<F>>
		requires(not std::same_as<function_ref, std::remove_cvref_t<F>> and is_invocable_using<T &>)
		function_ref(F && func) noexcept {
			self = std::addressof(func);
			disp = +[](void * self, Args... args) -> Result { return (*reinterpret_cast<T *>(self))(std::forward<Args>(args)...); };
		}

		function_ref(const function_ref &) noexcept =default;
		auto operator=(const function_ref &) noexcept -> function_ref & =default;

		template<typename T>
		requires(not std::same_as<T, function_ref>)
		auto operator=(T) -> function_ref & =delete;

		auto operator()(Args... args) const -> Result { return disp(self, std::forward<Args>(args)...); }
	};

	template<typename Result, typename... Args>
	class function_ref<Result(Args...) const noexcept> final {
		template<typename T>
		static
		constexpr
		bool is_invocable_using{std::is_nothrow_invocable_r_v<Result, T, Args...>};

		const void * self;
		Result(*disp)(const void *, Args...) noexcept;
	public:
		template<typename F, typename T = std::remove_reference_t<F>>
		requires(not std::same_as<function_ref, std::remove_cvref_t<F>> and is_invocable_using<const T &>)
		function_ref(F && func) noexcept {
			self = std::addressof(func);
			disp = +[](const void * self, Args... args) noexcept -> Result { return (*reinterpret_cast<const T *>(self))(std::forward<Args>(args)...); };
		}

		function_ref(const function_ref &) noexcept =default;
		auto operator=(const function_ref &) noexcept -> function_ref & =default;

		template<typename T>
		requires(not std::same_as<T, function_ref>)
		auto operator=(T) -> function_ref & =delete;

		auto operator()(Args... args) const noexcept -> Result { return disp(self, std::forward<Args>(args)...); }
	};
#else
	template<typename Signature>
	using function_ref = std::function_ref<Signature>;
#endif
}
#if defined(__cpp_lib_execution) and defined(__cpp_lib_parallel_algorithm)
	#include <execution>
	#include <algorithm>

	namespace lazy::compat {
		template<typename R, typename Func>
		void parallel_for_each(R && range, Func func) noexcept {
			//TODO: [C++26] use std::ranges::for_each(std::execution::par, tasks
			std::for_each(std::execution::par, std::begin(range), std::end(range), func);
		}
	}
#else
	#warning std::execution or parallel algorithms are not supported by your implementation, using TBB as transitional solution.
	#include <oneapi/tbb/parallel_for_each.h>

	namespace lazy::compat {
		template<typename R, typename Func>
		void parallel_for_each(R && range, Func func) noexcept {
			oneapi::tbb::parallel_for_each(std::begin(range), std::end(range), func);
		}
	}
#endif

namespace lazy {
	using clock = std::chrono::steady_clock;
	using duration = clock::duration;

	template<typename>
	struct generator;

	template<typename>
	struct task;

	//! @brief level of logging, higher levels include all messages of lower levels
	enum class log_level {
		fatal, //!< log only fatal events, reserved for internal errors
		error,
		warning,
		info,
		debug,
		trace, //!< reserved for dumping
	};

	//! @brief entry in log
	struct log_message final {
		//! @brief code location the message was created
		//! @note due to the way coroutines are expanded, this may not be an exact location
		std::source_location location;
		log_level level;
		//! @note if @code{.cpp} level == log_level::trace @endcode, then @c data is: @c [filename\0content], else it is the log mesage
		std::string data;
	};

	//! @brief tag to time wall clock of execution of a @c task
	//! @note the return type @c R of @c co_await timed{task<T>} is:
	//! If @code{.cpp} T == void @endcode then: @code{.cpp} R == duration @endcode ,
	//! Else: @code{.cpp} R == std::pair<duration, T> @endcode
	template<typename T>
	struct timed final { task<T> t; };

	//! @brief tag to yield all elements of a @c generator
	//! @note no support for general ranges, as those are not really compatible with lazy model
	template<typename T>
	struct elements_of final { generator<T> g; };

	namespace internal {
		class root_data;
	}

	//! @brief unique identifier of a coroutine stack
	//! @note once a coroutine stack has finished, another coroutine stack may re-use the same id
	class id final {
		std::uintptr_t val{0};

		explicit
		id(std::uintptr_t val) noexcept : val{val} {}

		friend
		internal::root_data;
		friend
		std::hash<id>;
		friend
		std::formatter<id>;
	public:
		id() noexcept =default;

		friend
		auto operator<=>(id, id) noexcept =default;
	};
}

namespace std {
	template<>
	struct hash<lazy::id> {
		auto operator()(lazy::id self) const noexcept -> std::size_t {
			return hash<std::uintptr_t>{}(self.val);
		}
	};

	template<>
	struct formatter<lazy::id> : formatter<std::uintptr_t> {
		auto format(lazy::id self, auto & ctx) const {
			return formatter<std::uintptr_t>::format(self.val, ctx);
		}
	};
}

namespace lazy {
	namespace internal {
		template<typename Promise>
		class unique_handle final {
			std::coroutine_handle<Promise> handle;
		public:
			unique_handle() noexcept =default;
			unique_handle(std::coroutine_handle<Promise> h) noexcept : handle{h} {}
			unique_handle(unique_handle && other) noexcept : handle{std::exchange(other.handle, {})} {}
			auto operator=(unique_handle && other) noexcept -> unique_handle & {
				std::swap(handle, other.handle);
				return *this;
			}
			~unique_handle() noexcept { if(handle) handle.destroy(); }

			auto done() const pre(handle) { return handle.done(); }
			void resume() pre(not done()) { handle.resume(); }
			auto promise() const -> Promise & pre(handle) { return handle.promise(); }

			explicit
			operator bool() const noexcept { return static_cast<bool>(handle); }
			operator std::coroutine_handle<>() const noexcept { return handle; }
		};

		//! @brief base for all awaiters that will be passed through @c await_transform transparently
		struct await_base : std::suspend_always {};

		//! @brief base for all awaiters that will be passed through @c yield_value transparently
		struct yield_base : std::suspend_always {};

		class root_data final {
			const
			struct vtable final {
				duration(*elapsed)(const void *) noexcept;
				bool(*is_tracing)(const void *) noexcept;
				void(*add_log_message)(void *, std::source_location, log_level, compat::function_ref<std::string()>);
			} * vptr;
			void * self;
		public:
			auto elapsed() const noexcept -> duration { return vptr->elapsed(self); }
			auto is_tracing() const noexcept -> bool { return vptr->is_tracing(self); }
			auto add_log_message(std::source_location loc, log_level log, compat::function_ref<std::string()> msg) { vptr->add_log_message(self, loc, log, msg); }

			template<typename T>
			root_data(T && obj) : self{std::addressof(obj)} {
				using U = std::remove_reference_t<T>;
				static constexpr vtable vtable{
					+[](const void * self) noexcept { return reinterpret_cast<const U *>(self)->timer.elapsed(); },
					+[](const void * self) noexcept { return reinterpret_cast<const U *>(self)->level == log_level::trace; },
					+[](void * self, std::source_location loc, log_level log, compat::function_ref<std::string()> msg) {
						auto ptr{reinterpret_cast<U *>(self)};
						if(log <= ptr->level) {
							for(auto expected{false}; not ptr->message_lock.compare_exchange_weak(expected, true); expected = false);
							const struct guard final { std::atomic<bool> & flag; ~guard() noexcept { flag = false; } } g{ptr->message_lock}; //defer...
							ptr->messages.emplace_back(loc, log, msg());
						}
					}
				};
				vptr = &vtable;
			}

			auto get_id() const noexcept { return id{reinterpret_cast<std::uintptr_t>(static_cast<const void *>(this))}; }

			std::coroutine_handle<> top;
			compat::function_ref<bool() const noexcept> suspend{std::false_type{}};
		private:
			bool blocked_{false}, has_result_{false};
		public:
			void reset_state() noexcept { blocked_ = has_result_ = false; }

			void set_blocked() noexcept { blocked_ = true; }
			auto blocked() const noexcept -> bool { return blocked_; }

			void set_has_result() noexcept { has_result_ = true; }
			auto has_result() const noexcept -> bool { return has_result_; }
		};

		class promise_base;

		struct nested_info final {
			std::exception_ptr eptr;        //needed for manual stack unwinding
			std::coroutine_handle<> parent; //directly preceding coroutine
			promise_base * root;            //bottom of implicit coroutine-"stack"
		};

		class promise_base {
			//! @attention tagged "union"
			//! LSB set => nested_info *
			//! LSB + 1 set => root_data * (=> this promise is at bottom of "stack")
			//! else @c void* obtained from @c std::coroutine_handle<>::address of top-coroutine
			std::uintptr_t data;
		public:
			void set_nested(nested_info & nested) { data = reinterpret_cast<std::uintptr_t>(&nested) | 1U; }
			void set_top(std::coroutine_handle<> handle) { data = reinterpret_cast<std::uintptr_t>(handle.address()); }
			void set_root(root_data & rd) { data = reinterpret_cast<std::uintptr_t>(std::addressof(rd)) | 2U; }

			auto get_nested() const -> nested_info * pre(not (data & 2U)) { return (data & 1U) ? reinterpret_cast<nested_info *>(data ^ 1U) : nullptr; }
			auto get_top() const -> std::coroutine_handle<> pre(not (data & 3U)) { return std::coroutine_handle<>::from_address(reinterpret_cast<void *>(data)); }
			auto get_root() const -> root_data & pre(not (data & 1U) and (data & 2U)) { return *reinterpret_cast<root_data *>(data ^ 2U); }

			auto find_root() const -> root_data & {
				if(auto nested{get_nested()}) return nested->root->get_root();
				else return get_root();
			}

			//TODO: [C++26] generate get_return_object() with deducing this and reflection

			static
			auto initial_suspend() noexcept { return std::suspend_always{}; }
		private:
			struct pop_awaiter final : std::suspend_always {
				template<typename Promise>
				auto await_suspend(std::coroutine_handle<Promise> self) noexcept -> std::coroutine_handle<> {
					if(const auto nested{self.promise().get_nested()}) {
						auto & rd{nested->root->get_root()};
						rd.top = nested->parent;
						if(not rd.suspend()) return rd.top;
					}
					return std::noop_coroutine();
				}
			};
		public:
			static
			auto final_suspend() noexcept { return pop_awaiter{}; }

			void unhandled_exception(std::source_location loc = std::source_location::current()) {
				if(auto n{get_nested()}) n->eptr = std::current_exception();
				else {
					//! @note loc will not identify the actual throw-site, due to the coroutine body transformation
					auto & rd{get_root()};
					try { throw; }
					catch(const std::exception & exc) { rd.add_log_message(loc, log_level::fatal, [&] { return std::string{exc.what()}; }); }
					catch(...) { rd.add_log_message(loc, log_level::fatal, [] { return std::string{"unknown error"}; }); }
					throw;
				}
			}
		private:
			template<typename T, bool Timed>
			class push_awaiter final : public std::suspend_always {
				nested_info n;
				unique_handle<T> other;
				//! @note only accessed when @code{.cpp} Timed == true @endcode
				duration elapsed;
			public:
				push_awaiter(unique_handle<T> other) pre(other and not other.done()) : other{std::move(other)} {}

				template<typename Promise>
				auto await_suspend(std::coroutine_handle<Promise> self) noexcept -> std::coroutine_handle<> {
					other.promise().set_nested(n);
					n.parent = self;
					auto nested{self.promise().get_nested()};
					n.root = nested ? nested->root : std::addressof(self.promise());
					auto & rd{n.root->get_root()};
					rd.top = other;
					if constexpr(Timed) elapsed = rd.elapsed();
					if(rd.suspend()) return std::noop_coroutine();
					else return rd.top;
				}

				auto await_resume() pre(other.done()) {
					if(n.eptr) std::rethrow_exception(n.eptr);
					if constexpr(Timed) {
						auto & rd{n.root->get_root()};
						elapsed = rd.elapsed() - elapsed;
					}

					auto & promise{other.promise()};
					if constexpr(requires { promise.get_result(); }) { //! @note @c task<T> where @code{.cpp} T != void @endcode
						if constexpr(Timed) return std::make_pair(elapsed, std::move(promise.get_result()));
						else return std::move(promise.get_result());
					} else { //! @note @c generator or @c task<void>
						if constexpr(Timed) return elapsed;
					}
				}
			};

			template<bool Timed, typename T>
			static
			auto push(unique_handle<T> handle) pre(handle and not handle.done()) { return push_awaiter<T, Timed>{std::move(handle)}; }
		public:
			template<typename T>
			static
			auto await_transform(task<T> other) pre(not other.valueless()) { return push<false>(std::move(other.handle)); }

			template<typename T>
			static
			auto await_transform(timed<T> other) pre(not other.t.valueless()) { return push<true>(std::move(other.t.handle)); }

			static
			auto await_transform(std::derived_from<await_base> auto a) noexcept { return a; }

			template<typename Self, typename T>
			requires std::same_as<Self, typename generator<T>::promise_type>
			auto yield_value(this Self & self, elements_of<T> other) pre(not other.g.valueless() and not other.g.handle.promise()) {
				other.g.handle.promise() = self;
				return push<false>(std::move(other.g.handle));
			}

			static
			auto yield_value(std::derived_from<yield_base> auto a) noexcept { return a; }
		private:
			//memory layout:
			//     [ coroutine frame ] [ deleter ]   [ offset ] [ padding  ] [ allocator ]
			//     [       ? B       ] [   8 B   ]   [   1 B  ] [ offset B ] [    ? B    ]
			//     [--      always present     --]   [--   only for statefull alloc    --]

			using deleter_t = void(*)(void *, std::size_t) noexcept;
			static_assert(sizeof(deleter_t) == sizeof(std::uintptr_t));

			template<typename Alloc>
			static
			auto allocate(std::size_t size, const Alloc * alloc, ...) -> void * {
				contract_assert(alloc);
				using A = std::allocator_traits<Alloc>::template rebind_alloc<std::byte>;

				constexpr auto stateless_allocator{std::is_default_constructible_v<A> and std::allocator_traits<A>::is_always_equal::value};

				const auto offset{stateless_allocator ? 0 : ((alignof(A) - (size + sizeof(deleter_t) + 1) % alignof(A)) % alignof(A))};
				contract_assert(offset <= 255);
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
				A a{*alloc};
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
			auto operator new(std::size_t size) -> void * {
				std::allocator<char> alloc;
				return allocate(size, std::addressof(alloc));
			}

			//! @note allocator, non-member function
			static
			auto operator new(std::size_t size, std::allocator_arg_t, const auto &... args) -> void * {
				static_assert(sizeof...(args), "if allocator_arg_t is first argument, the second argument must be an allocator");
				return allocate(size, std::addressof(args)...);
			}

			//! @note allocator, member function
			static
			auto operator new(std::size_t size, const auto &, std::allocator_arg_t, const auto &... args) -> void * {
				static_assert(sizeof...(args), "if allocator_arg_t is first argument, the second argument must be an allocator");
				return allocate(size, std::addressof(args)...);
			}

			//! @note must handle all versions of @code{.cpp} operator new() @endcode
			static
			void operator delete(void * ptr, std::size_t size) noexcept {
				std::uintptr_t d;
				std::memcpy(&d, static_cast<char *>(ptr) + size, sizeof(std::uintptr_t));
				contract_assert(d);
				reinterpret_cast<deleter_t>(d)(static_cast<std::byte *>(ptr), size);
			}
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
			void return_value(U && value) {
				new(std::addressof(result)) T(std::forward<U>(value));
				initialized = true;
				if(const auto nested{get_nested()}; not nested) get_root().set_has_result();
			}

			auto get_result() -> T & pre(initialized) { return result; }
		};

		template<>
		struct task_promise<void> : promise_base {
			static
			void return_void() noexcept {}
		};

		struct progress_t final : yield_base {
			template<typename Promise>
			auto await_suspend(std::coroutine_handle<Promise> self) const noexcept { return self.promise().find_root().suspend(); }
		};

		struct blocked_t final : yield_base {
			template<typename Promise>
			void await_suspend(std::coroutine_handle<Promise> self) const noexcept { self.promise().find_root().set_blocked(); }
		};

		class get_identity_t final : public await_base {
			id result;
		public:
			template<typename Promise>
			auto await_suspend(std::coroutine_handle<Promise> self) noexcept -> bool {
				const auto & rd{self.promise().find_root()};
				result = rd.get_id();
				return rd.suspend();
			}
			auto await_resume() const noexcept -> id { return result; }
		};

		class get_is_tracing_t final : public await_base {
			bool result{false};
		public:
			template<typename Promise>
			auto await_suspend(std::coroutine_handle<Promise> self) noexcept -> bool {
				const auto & rd{self.promise().find_root()};
				result = rd.is_tracing();
				return rd.suspend();
			}
			auto await_resume() const noexcept -> bool { return result; }
		};

		template<typename... Args>
		class log_message : public await_base {
			log_level level;
			std::string_view fmt;
			decltype(std::make_format_args(std::declval<Args>()...)) args;
			std::source_location loc;
		public:
			log_message(log_level level, std::format_string<Args...> fmt, Args &&... args, std::source_location loc) noexcept : level{level}, fmt{fmt.get()}, args{std::make_format_args(args...)}, loc{loc} {}

			template<typename Promise>
			auto await_suspend(std::coroutine_handle<Promise> self) const -> bool {
				auto & rd{self.promise().find_root()};
				rd.add_log_message(loc, level, [&] { return std::vformat(fmt, args); });
				return rd.suspend();
			}
		};
	}

	//! @brief awaiter to request identity of root of coroutine stack
	inline
	constexpr
	internal::get_identity_t get_identity;

	//! @brief awaiter to request whether the active @c log_level is @c trace
	inline
	constexpr
	internal::get_is_tracing_t get_is_tracing;

	//! @brief awaiter to yield progress
	inline
	constexpr
	internal::progress_t progress;

	//! @brief awaiter to yield progress and signal that progress is blocked by a synchronization primitive
	//! @attention should only be used when implementing synchronization primitives
	inline
	constexpr
	internal::blocked_t blocked;

	//! @brief awaiter to create error-log entry
	template<typename... Args>
	struct [[nodiscard]] error final : internal::log_message<Args...> {
		error(std::format_string<Args...> fmt, Args &&... args, std::source_location loc = std::source_location::current()) noexcept : internal::log_message<Args...>{log_level::error, fmt, std::forward<Args>(args)..., loc} {}
	};
	template<typename... Args>
	error(std::format_string<Args...>, Args &&...) -> error<Args...>;

	//! @brief awaiter to create warning-log entry
	template<typename... Args>
	struct [[nodiscard]] warning final : internal::log_message<Args...> {
		warning(std::format_string<Args...> fmt, Args &&... args, std::source_location loc = std::source_location::current()) noexcept : internal::log_message<Args...>{log_level::warning, fmt, std::forward<Args>(args)..., loc} {}
	};
	template<typename... Args>
	warning(std::format_string<Args...>, Args &&...) -> warning<Args...>;

	//! @brief awaiter to create info-log entry
	template<typename... Args>
	struct [[nodiscard]] info final : internal::log_message<Args...> {
		info(std::format_string<Args...> fmt, Args &&... args, std::source_location loc = std::source_location::current()) noexcept : internal::log_message<Args...>{log_level::info, fmt, std::forward<Args>(args)..., loc} {}
	};
	template<typename... Args>
	info(std::format_string<Args...>, Args &&...) -> info<Args...>;

	//! @brief awaiter to create debug-log entry
	template<typename... Args>
	struct [[nodiscard]] debug final : internal::log_message<Args...> {
		debug(std::format_string<Args...> fmt, Args &&... args, std::source_location loc = std::source_location::current()) noexcept : internal::log_message<Args...>{log_level::debug, fmt, std::forward<Args>(args)..., loc} {}
	};
	template<typename... Args>
	debug(std::format_string<Args...>, Args &&...) -> debug<Args...>;

	//! @brief base class for  awaiter to create trace-log entry
	class dump_base : public internal::await_base {
		//! @brief actual dumping logic, only called when @c log_level::trace is active
		virtual
		void dump_to(
			std::back_insert_iterator<std::string> out //!< [in] iterator to write dump content to
		) const =0;

		std::string_view file_name;
		std::source_location loc;
	public:
		dump_base(std::string_view file_name, std::source_location loc) noexcept : file_name{file_name}, loc{loc} {}

		template<typename Promise>
		auto await_suspend(std::coroutine_handle<Promise> self) const {
			auto & rd{self.promise().find_root()};
			rd.add_log_message(loc, log_level::trace, [&] {
				std::string msg{file_name};
				msg += '\0';
				dump_to(std::back_inserter(msg));
				return msg;
			});
			return rd.suspend();
		}
	};

	//! @brief root of coroutine stack
	//! @tparam Wrapper type of wrapper that is managed
	template<typename Wrapper>
	struct root;

	template<typename Wrapper>
	root(Wrapper) -> root<Wrapper>;
	template<typename Wrapper>
	root(log_level, Wrapper) -> root<Wrapper>;

	//! @brief state of coroutine stack when waiting ends
	enum class state {
		done,      //!< execution completed, @c task result may be ready
		suspended, //!< suspended due to timeout or user request, @c generator result may be ready
		blocked,   //!< suspended due to synchronization primitive
	};

	namespace internal {
		template<typename>
		inline
		constexpr
		bool is_task_v{false};

		template<typename T>
		inline
		constexpr
		bool is_task_v<task<T>>{true};

		template<typename T>
		concept task = is_task_v<T>;

		template<typename>
		struct task_result;

		template<typename T>
		using task_result_t = typename task_result<T>::type;

		template<typename T>
		struct task_result<lazy::task<T>> { using type = T; };

		template<typename From, typename To, typename Indices, std::size_t Index>
		struct tuple_erase_void;

		template<typename To, typename Is, std::size_t I>
		struct tuple_erase_void<std::tuple<>, To, Is, I> {
			using type = To;
			using indices = Is;
		};

		template<typename T, typename... From, typename... To, std::size_t... Is, std::size_t I>
		struct tuple_erase_void<std::tuple<T, From...>, std::tuple<To...>, std::index_sequence<Is...>, I> : std::conditional_t<std::is_void_v<T>, tuple_erase_void<std::tuple<From...>, std::tuple<To...>, std::index_sequence<Is...>, I + 1>, tuple_erase_void<std::tuple<From...>, std::tuple<To..., T>, std::index_sequence<Is..., I>, I + 1>> {};

		template<typename From>
		using tuple_erase_void_type = typename tuple_erase_void<From, std::tuple<>, std::index_sequence<>, 0>::type;

		template<typename From>
		using tuple_erase_void_indices = typename tuple_erase_void<From, std::tuple<>, std::index_sequence<>, 0>::indices;

		template<typename From, typename To>
		struct tuple_make_unique;

		template<typename To>
		struct tuple_make_unique<std::tuple<>, To> { using type = To; };

		template<typename T, typename... From, typename... To>
		struct tuple_make_unique<std::tuple<T, From...>, std::tuple<To...>> : tuple_make_unique<std::tuple<From...>, std::conditional_t<(std::is_same_v<T, To> or ...), std::tuple<To...>, std::tuple<To..., T>>> {};

		template<typename From>
		using tuple_make_unique_t = typename tuple_make_unique<From, std::tuple<>>::type;

		template<typename From>
		struct tuple_to_variant;

		template<typename... From>
		struct tuple_to_variant<std::tuple<From...>> { using type = std::variant<From...>; };

		template<typename From>
		using tuple_to_variant_t = typename tuple_to_variant<From>::type;

		template<typename...>
		struct compute_all_of_result;

		template<typename... Ts>
		using compute_all_of_result_t = typename compute_all_of_result<Ts...>::type;

		template<task... Ts>
		struct compute_all_of_result<Ts...> {
		private:
			using tmp0 = std::tuple<task_result_t<Ts>...>;
			using tmp1 = tuple_erase_void_type<tmp0>;
			using tmp2 = std::conditional_t<(std::tuple_size_v<tmp1> == 0), std::tuple<void>, tmp1>; //! @note prevent @code{.cpp} tuple_element_t<0, tmp1> @endcode from getting out of range
			using tmp3 = std::tuple_element_t<0, tmp2>;
		public:
			using type = std::conditional_t<(std::tuple_size_v<tmp1> > 1), tmp1, tmp3>;
		};

		template<std::ranges::range T, typename Alloc>
		struct compute_all_of_result<T, Alloc> {
		private:
			using tmp0 = std::ranges::range_value_t<T>;
			static_assert(task<tmp0>);
			using tmp1 = task_result_t<tmp0>;
			using tmp2 = std::conditional_t<std::is_void_v<tmp1>, int, tmp1>; //! @note prevent @c vector<void> from being constructed
			using alloc = std::allocator_traits<Alloc>::template rebind_alloc<tmp2>;
		public:
			using type = std::conditional_t<std::is_void_v<tmp1>, void, std::vector<tmp2, alloc>>;
		};

		template<typename...>
		struct compute_any_of_result;

		template<typename... Ts>
		using compute_any_of_result_t = typename compute_any_of_result<Ts...>::type;

		template<task... Ts>
		struct compute_any_of_result<Ts...> {
		private:
			static_assert(((not std::is_void_v<task_result_t<Ts>>) or ...));
			using tmp0 = std::tuple<task_result_t<Ts>...>;
			using tmp1 = tuple_make_unique_t<tmp0>;
		public:
			using type = std::conditional_t<(std::tuple_size_v<tmp1> == 1), std::tuple_element_t<0, tmp1>, tuple_to_variant_t<tmp1>>;
		};

		template<std::ranges::range T, typename Alloc>
		struct compute_any_of_result<T, Alloc> {
		private:
			using tmp0 = std::ranges::range_value_t<T>;
			static_assert(task<tmp0>);
		public:
			using type = task_result_t<tmp0>;
			static_assert(not std::is_void_v<type>);
		};

		class get_root_awaiter final : public await_base {
			const root_data * result;
		public:
			template<typename Promise>
			auto await_suspend(std::coroutine_handle<Promise> self) -> bool {
				result = std::addressof(self.promise().find_root());
				return false;
			}
			auto await_resume() const noexcept -> const root_data & { return *result; }
		};
	}

	//! @brief factory for @c tasks matching this signature: @code{.cpp} task<?>(std::allocator_arg_t, Alloc, std::size_t); @endcode
	template<typename F, typename Alloc>
	concept allocator_aware_task_factory = requires(F f, Alloc alloc, std::size_t index) {
		{ f(std::allocator_arg, alloc, index) } -> internal::task;
	};

	//! @brief factory for @c tasks matching this signature: @c task<?>(std::size_t);
	template<typename F>
	concept task_factory = requires(F f, std::size_t index) {
		{ f(index) } -> internal::task;
	};

	//! @brief helper to define a compile-time constant
	template<auto N, typename T = decltype(N)>
	inline
	constexpr
	std::integral_constant<T, N> cw;

	class all_of_t final {
		struct fork_data final {
			std::coroutine_handle<> bottom;
			internal::root_data rd;
		};

		static
		auto run(std::atomic<bool> & stop, std::span<fork_data> datas) {
			std::atomic<bool> blocked{false}, suspended{false};
			std::exception_ptr eptr; //! @note concurrent access guarded by @c stop

			compat::parallel_for_each(datas, [&](auto & data) {
				if(data.bottom.done()) return;
				data.rd.reset_state();

				try {
					data.rd.top.resume();
					if(not data.bottom.done()) (data.rd.blocked() ? blocked : suspended) = true;
				} catch(...) {
					if(auto expected{false}; stop.compare_exchange_strong(expected, true))
						eptr = std::current_exception();
				}
			});

			if(stop) {
				contract_assert(eptr);
				std::rethrow_exception(eptr);
			}

			if(not blocked and not suspended) return state::done; //! @note all tasks are done
			if(blocked and not suspended) return state::blocked;
			return state::suspended;
		}
	public:
		//! @returns a @c task managing the wrapped @c tasks, returning their results if any
		//! @note the return type @c R of the returned @c task is:
		//! Let @c W... be the result types of @c Tasks
		//! Let @c U... be @c W... with all instances of @c void removed
		//! If @code{.cpp} sizeof...(U) == 0 @endcode then @code{.cpp} R == void @endcode ,
		//! else if @code{.cpp} sizeof...(U) == 1 @endcode then @code{.cpp} R == U @endcode ,
		//! else @code{.cpp} R == std::tuple<U...> @endcode
		template<typename Alloc, typename... Tasks>
		requires(sizeof...(Tasks) >= 2 and (internal::task<Tasks> and ...))
		static
		auto operator()(std::allocator_arg_t, Alloc, Tasks... tasks) -> task<internal::compute_all_of_result_t<Tasks...>> pre((not tasks.valueless()) and ...) {
			const auto & root{co_await internal::get_root_awaiter{}};

			std::atomic<bool> stop{false};
			const auto suspend{[&] noexcept { return stop ? true : root.suspend(); }};

			auto handles{std::make_tuple(std::ref(tasks.handle)...)};
			std::array<fork_data, sizeof...(Tasks)> datas{fork_data{tasks.handle, root}...};
			[&]<auto... I>(std::index_sequence<I...>) {
				[[maybe_unused]] const int dummy0[]{((datas[I].rd.suspend = compat::function_ref<bool() const noexcept>{suspend}), 0)...};
				[[maybe_unused]] const int dummy1[]{((datas[I].rd.top = datas[I].bottom), 0)...};
				[[maybe_unused]] const int dummy2[]{((std::get<I>(handles).promise().set_root(datas[I].rd)), 0)...};
			}(std::index_sequence_for<Tasks...>{});

			for(;;) {
				switch(run(stop, datas)) {
					case state::suspended: co_yield progress; break;
					case state::blocked: co_yield blocked; break;
					case state::done: {
						using Indices = internal::tuple_erase_void_indices<std::tuple<internal::task_result_t<Tasks>...>>;
						if constexpr(Indices::size() == 0) co_return;
						else if constexpr(Indices::size() == 1) co_return [&]<auto I>(std::index_sequence<I>) { return std::move(std::get<I>(handles).promise().get_result()); }(Indices{});
						else co_return [&]<auto... I>(std::index_sequence<I...>) { return std::make_tuple(std::move(std::get<I>(handles).promise().get_result())...); }(Indices{});
					} break;
				}
			}
		}

		template<typename... Tasks>
		requires(sizeof...(Tasks) >= 2 and (internal::task<Tasks> and ...))
		static
		auto operator()(Tasks... tasks) { return all_of_t{}(std::allocator_arg, std::allocator<char>{}, std::move(tasks)...); }

		//! @returns a @c task managing the wrapped @c tasks, returning their results if any
		//! @note the return type @c R of the returned @c task is:
		//! Let @c T be the result type of @c Tasks
		//! If @code{.cpp} T == void @endcode then @code{.cpp} R == T @endcode ,
		//! otherwise @code{.cpp} R == std::vector<T, Alloc> @endcode
		template<typename Alloc, std::ranges::forward_range Tasks>
		requires internal::task<std::ranges::range_value_t<Tasks>>
		static
		auto operator()(std::allocator_arg_t, Alloc alloc, Tasks tasks) -> task<internal::compute_all_of_result_t<Tasks, Alloc>> pre(std::ranges::none_of(tasks, [](const auto & t) { return t.valueless(); })) {
			const auto & root{co_await internal::get_root_awaiter{}};

			std::atomic<bool> stop{false};
			const auto suspend{[&] noexcept { return stop ? true : root.suspend(); }};

			auto datas{tasks | std::views::transform([&](const auto & task) { return fork_data{task.handle, root}; })
							 | std::ranges::to<std::vector<fork_data, typename std::allocator_traits<Alloc>::template rebind_alloc<fork_data>>>(alloc)};
			for(auto && [task, data] : std::views::zip(tasks, datas)) {
				data.rd.suspend = compat::function_ref<bool() const noexcept>{suspend};
				data.rd.top = data.bottom;
				task.handle.promise().set_root(data.rd);
			}

			for(;;) {
				switch(run(stop, datas)) {
					case state::suspended: co_yield progress; break;
					case state::blocked: co_yield blocked; break;
					case state::done: {
						using Result = internal::compute_all_of_result_t<Tasks, Alloc>;
						if constexpr(std::is_void_v<Result>) co_return;
						else co_return tasks | std::views::transform([](auto & task) { return std::move(task.handle.promise().get_result()); }) | std::ranges::to<Result>(alloc);
					} break;
				}
			}
		}

		template<std::ranges::forward_range Tasks>
		requires internal::task<std::ranges::range_value_t<Tasks>>
		static
		auto operator()(Tasks tasks) { return all_of_t{}(std::allocator_arg, std::allocator<char>{}, std::move(tasks)); }

		//! @brief create multiple @c tasks and execute them in parallel
		//! @tparam N count of @c tasks to create
		template<typename Alloc, std::integral T, T N>
		requires(N >= 2)
		static
		auto operator()(std::allocator_arg_t, Alloc alloc, std::integral_constant<T, N>, allocator_aware_task_factory<Alloc> auto f) { return [&]<auto... I>(std::index_sequence<I...>) { return all_of_t{}(std::allocator_arg, alloc, std::invoke(f, std::allocator_arg, alloc, I)...); }(std::make_index_sequence<N>{}); }

		template<std::integral T, T N>
		requires(N >= 2)
		static
		auto operator()(std::integral_constant<T, N>, task_factory auto f) { return [&]<auto... I>(std::index_sequence<I...>) { return all_of_t{}(std::invoke(f, I)...); }(std::make_index_sequence<N>{}); }

		//! @brief create multiple @c tasks and execute them in parallel
		template<typename Alloc>
		static
		auto operator()(std::allocator_arg_t, Alloc alloc, std::size_t count, allocator_aware_task_factory<Alloc> auto f) {
			using Task = decltype(f(std::allocator_arg, alloc, std::size_t{}));
			std::vector<Task, typename std::allocator_traits<Alloc>::template rebind_alloc<Task>> tasks(alloc);
			tasks.reserve(count);
			for(std::size_t i{0}; i < count; ++i) tasks.emplace_back(f(std::allocator_arg, alloc, i));
			return all_of_t{}(std::allocator_arg, alloc, std::move(tasks));
		}

		static
		auto operator()(std::size_t count, task_factory auto f) {
			std::vector<decltype(f(std::size_t{}))> tasks;
			tasks.reserve(count);
			for(std::size_t i{0}; i < count; ++i) tasks.emplace_back(f(i));
			return all_of_t{}(std::move(tasks));
		}
	};

	//! @brief execute multiple @c tasks in parallel, waiting for all of them to run to completion
	inline
	constexpr
	all_of_t all_of;

	class any_of_t final {
		struct fork_data final {
			std::coroutine_handle<> bottom; //! @note default-constructed == done and yielded result to caller
			internal::root_data rd;
		};

		template<bool IgnoreExceptions>
		static
		auto run(std::atomic<bool> & stop, std::span<fork_data> datas) {
			std::atomic<bool> blocked{false}, suspended{false}, done{false};

			//! @note only relevant for @code{.cpp} not IgnoreExceptions @endcode
			std::atomic<bool> error{false};
			std::exception_ptr eptr; //! @note guarded by @c error

			compat::parallel_for_each(datas, [&](auto & data) {
				if(not data.bottom) return;
				contract_assert(not data.bottom.done());
				data.rd.reset_state();

				try {
					data.rd.top.resume();
					if(data.bottom.done()) stop = done = true;
					else (data.rd.blocked() ? blocked : suspended) = true;
				} catch(...) {
					if constexpr(IgnoreExceptions) {
						data.bottom = std::coroutine_handle<>{};
						done = true;
					} else {
						if(auto expected{false}; error.compare_exchange_strong(expected, true)) eptr = std::current_exception();
						stop = true;
					}
				}
			});

			if constexpr(not IgnoreExceptions) {
				if(eptr) std::rethrow_exception(eptr);
			}

			if(done) return state::done; //! @note at least one task sucessfully done ...
			if(blocked and not suspended) return state::blocked;
			return state::suspended;
		}
	public:
		//! @returns a @c task managing the wrapped @c tasks, returning their results
		//! @note the return type @c R of the returned @c task is:
		//! Let @c V... be the result types of @c Tasks
		//! Let @c U... be @c V... with all duplicated types removed
		//! If @code{.cpp} sizeof...(U) == 1 @endcode then @code{.cpp} R == U @endcode ,
		//! Else: @code{.cpp} R == std::variant<U...> @endcode ,
		//! @attention @code{.cpp} V... != void @endcode
		template<typename Alloc, bool IgnoreExceptions, typename... Tasks>
		requires(sizeof...(Tasks) >= 2 and (internal::task<Tasks> and ...))
		static
		auto operator()(std::allocator_arg_t, Alloc, std::bool_constant<IgnoreExceptions>, Tasks... tasks) -> generator<internal::compute_any_of_result_t<Tasks...>> pre((not tasks.valueless()) and ...) {
			const auto & root{co_await internal::get_root_awaiter{}};

			std::atomic<bool> stop{false};
			const auto suspend{[&] noexcept { return stop ? true : root.suspend(); }};

			auto handles{std::make_tuple(std::ref(tasks.handle)...)};
			std::array<fork_data, sizeof...(Tasks)> datas{fork_data{tasks.handle, root}...};
			[&]<auto... I>(std::index_sequence<I...>) {
				[[maybe_unused]] const int dummy0[]{((datas[I].rd.suspend = compat::function_ref<bool() const noexcept>{suspend}), 0)...};
				[[maybe_unused]] const int dummy1[]{((datas[I].rd.top = datas[I].bottom), 0)...};
				[[maybe_unused]] const int dummy2[]{((std::get<I>(handles).promise().set_root(datas[I].rd)), 0)...};
			}(std::index_sequence_for<Tasks...>{});

			using Result = internal::compute_any_of_result_t<Tasks...>;
			for(;; stop = false) {
				switch(run<IgnoreExceptions>(stop, datas)) {
					case state::suspended: co_yield progress; break;
					case state::blocked: co_yield blocked; break;
					case state::done: {
						for(std::size_t index{0}; index < datas.size(); ++index) {
							if(datas[index].bottom and datas[index].bottom.done()) {
								co_yield [&]<auto... I>(std::index_sequence<I...>) {
									using Handles = decltype(handles);
									using Dispatch = Result(*)(Handles &);
									constexpr Dispatch disp[]{+[](Handles & h) -> Result {
										auto & promise{std::get<I>(h).promise()};
										if constexpr(requires { promise.get_result(); }) return std::move(promise.get_result());
										else static_assert(std::is_void_v<Result>);
									}...};
									return disp[index](handles);
								}(std::index_sequence_for<Tasks...>{});
								datas[index].bottom = std::coroutine_handle<>{};
							}
						}
						if(std::ranges::all_of(datas, [](const auto & d) -> bool { return not d.bottom; })) co_return;
					} break;
				}
			}
		}

		template<bool IgnoreExceptions, typename... Tasks>
		requires(sizeof...(Tasks) >= 2 and (internal::task<Tasks> and ...))
		static
		auto operator()(std::bool_constant<IgnoreExceptions> ignore, Tasks... tasks) { return any_of_t{}(std::allocator_arg, std::allocator<char>{}, ignore, std::move(tasks)...); }

		//! @returns a @c task managing the wrapped @c tasks, returning their results
		//! @note the return type @c R of the returned @c task is:
		//! Let @c T be the result type of @c Tasks
		//! @code{.cpp} R == T @endcode ,
		//! @attention @code{.cpp} T != void @endcode
		template<typename Alloc, bool IgnoreExceptions, std::ranges::forward_range Tasks>
		requires internal::task<std::ranges::range_value_t<Tasks>>
		static
		auto operator()(std::allocator_arg_t, Alloc alloc, std::bool_constant<IgnoreExceptions>, Tasks tasks) -> generator<internal::compute_any_of_result_t<Tasks, Alloc>> pre(std::ranges::none_of(tasks, [](const auto & t) { return t.valueless(); })) {
			const auto & root{co_await internal::get_root_awaiter{}};

			std::atomic<bool> stop{false};
			const auto suspend{[&] noexcept { return stop ? true : root.suspend(); }};

			auto datas{tasks | std::views::transform([&](const auto & task) { return fork_data{task.handle, root}; })
							 | std::ranges::to<std::vector<fork_data, typename std::allocator_traits<Alloc>::template rebind_alloc<fork_data>>>(alloc)};
			for(auto && [task, data] : std::views::zip(tasks, datas)) {
				data.rd.suspend = compat::function_ref<bool() const noexcept>{suspend};
				data.rd.top = data.bottom;
				task.handle.promise().set_root(data.rd);
			}

			for(;; stop = false) {
				switch(run<IgnoreExceptions>(stop, datas)) {
					case state::suspended: co_yield progress; break;
					case state::blocked: co_yield blocked; break;
					case state::done: {
						for(std::size_t index{0}; index < datas.size(); ++index) {
							if(datas[index].bottom and datas[index].bottom.done()) {
								co_yield std::move(tasks[index].handle.promise().get_result());
								datas[index].bottom = std::coroutine_handle<>{};
							}
						}
						if(std::ranges::all_of(datas, [](const auto & d) -> bool { return not d.bottom; })) co_return;
					} break;
				}
			}
		}

		template<bool IgnoreExceptions, std::ranges::forward_range Tasks>
		requires internal::task<std::ranges::range_value_t<Tasks>>
		static
		auto operator()(std::bool_constant<IgnoreExceptions> ignore, Tasks tasks) { return any_of_t{}(std::allocator_arg, std::allocator<char>{}, ignore, std::move(tasks)); }

		//! @brief create multiple @c tasks and execute them in parallel
		//! @tparam N count of @c tasks to create
		template<typename Alloc, std::integral T, T N, bool IgnoreExceptions>
		requires(N >= 2)
		static
		auto operator()(std::allocator_arg_t, Alloc alloc, std::bool_constant<IgnoreExceptions> ignore, std::integral_constant<T, N>, allocator_aware_task_factory<Alloc> auto f) { return [&]<auto... I>(std::index_sequence<I...>) { return any_of_t{}(std::allocator_arg, alloc, ignore, std::invoke(f, std::allocator_arg, alloc, I)...); }(std::make_index_sequence<N>{}); }

		template<std::integral T, T N, bool IgnoreExceptions>
		requires(N >= 2)
		static
		auto operator()(std::bool_constant<IgnoreExceptions> ignore, std::integral_constant<T, N>, task_factory auto f) { return [&]<auto... I>(std::index_sequence<I...>) { return any_of_t{}(ignore, std::invoke(f, I)...); }(std::make_index_sequence<N>{}); }

		//! @brief create multiple @c tasks and execute them in parallel
		template<typename Alloc, bool IgnoreExceptions>
		static
		auto operator()(std::allocator_arg_t, Alloc alloc, std::bool_constant<IgnoreExceptions> ignore, std::size_t count, allocator_aware_task_factory<Alloc> auto f) {
			using Task = decltype(f(std::allocator_arg, alloc, std::size_t{}));
			std::vector<Task, typename std::allocator_traits<Alloc>::template rebind_alloc<Task>> tasks(alloc);
			tasks.reserve(count);
			for(std::size_t i{0}; i < count; ++i) tasks.emplace_back(f(std::allocator_arg, alloc, i));
			return any_of_t{}(std::allocator_arg, alloc, ignore, std::move(tasks));
		}

		template<bool IgnoreExceptions>
		static
		auto operator()(std::bool_constant<IgnoreExceptions> ignore, std::size_t count, task_factory auto f) {
			std::vector<decltype(f(std::size_t{}))> tasks;
			tasks.reserve(count);
			for(std::size_t i{0}; i < count; ++i) tasks.emplace_back(f(i));
			return any_of_t{}(ignore, std::move(tasks));
		}
	};

	//! @brief execute multiple @c tasks in parallel, waiting for any one of them running to completion
	//! @attention in case a @c task ends with an exception, it is ignored and execution continues for the remaining @c tasks
	//template<bool IgnoreExceptions = true>
	inline
	constexpr
	any_of_t any_of;

	//! @brief cooperative synchronous(!) recursive coroutine task
	//! @tparam Result return type of the task
	//! additional supported coroutine statements:
	//!  * @code{.cpp} co_return [val]; @endcode to terminate the task and optionally return a value to the caller
	template<typename Result = void>
	struct [[nodiscard]] task final {
		static_assert(std::is_void_v<Result> or (std::is_object_v<Result> and std::is_same_v<std::decay_t<Result>, Result>));

		struct promise_type final : internal::task_promise<Result> {
			promise_type() { this->set_top(std::coroutine_handle<promise_type>::from_promise(*this)); }

			auto get_return_object() noexcept { return task{std::coroutine_handle<promise_type>::from_promise(*this)}; }
		};

		auto valueless() const noexcept -> bool { return not handle; }
	private:
		friend
		internal::promise_base;
		friend
		root<task>;
		friend
		all_of_t;
		friend
		any_of_t;

		task(std::coroutine_handle<promise_type> handle) noexcept : handle{handle} {}

		internal::unique_handle<promise_type> handle;
	};

	//! @brief cooperative synchronous(!) recursive coroutine generator
	//! @tparam Result return type of the generator
	//! additional supported coroutine statements:
	//!  * @code{.cpp} co_yield val; @endcode yield value to caller of generator
	//!  * @code{.cpp} co_yield elements_of{generator}; @endcode yield elements of @c generator
	//!  * @code{.cpp} co_return; @endcode to terminate the generator
	template<typename Result>
	struct [[nodiscard]] generator final {
		static_assert(std::is_object_v<Result> and std::is_same_v<std::decay_t<Result>, Result>);

		class promise_type final : public internal::promise_base {
			std::coroutine_handle<> cont;
		public:
			void set_continuation(std::coroutine_handle<> handle) noexcept { cont = handle; }
			auto get_continuation() const -> std::coroutine_handle<> {
				if(cont) contract_assert(not cont.done());
				return cont;
			}

			Result * ptr;

			promise_type() { this->set_top(std::coroutine_handle<promise_type>::from_promise(*this)); }

			auto get_return_object() noexcept { return generator{std::coroutine_handle<promise_type>::from_promise(*this)}; }

			using internal::promise_base::yield_value;

			auto yield_value(const Result & lval) requires std::is_copy_constructible_v<Result> {
				struct awaiter final : std::suspend_always {
					Result val;

					//! @note does not check for suspension, as we need to jump back to @c yield_target
					auto await_suspend(std::coroutine_handle<promise_type> self) noexcept -> std::coroutine_handle<> {
						auto & promise{self.promise()};
						promise.ptr = std::addressof(val);
						if(const auto cont{promise.get_continuation()}) return cont;
						else {
							promise.find_root().set_has_result();
							return std::noop_coroutine();
						}
					}
				};
				return awaiter{{}, lval};
			}

			auto yield_value(Result && val) noexcept {
				struct awaiter final : std::suspend_always {
					std::coroutine_handle<> continuation;

					//! @note does not check for suspension, as we need to jump back to @c yield_target
					auto await_suspend(std::coroutine_handle<>) const noexcept { return continuation; }
				};

				ptr = std::addressof(val);
				if(const auto cont{get_continuation()}) return awaiter{{}, cont};
				else {
					find_root().set_has_result();
					return awaiter{{}, std::noop_coroutine()};
				}
			}

			static
			void return_void() noexcept {}
		};
	private:
		//! @brief lazy iterator for elements yielded by a coroutine
		struct iterator final {
			using value_type = Result;
			using difference_type = std::ptrdiff_t;

			auto operator*() const -> Result && pre(handle and not handle.done()) {
				auto & promise{handle.promise()};
				auto top{std::coroutine_handle<promise_type>::from_address(promise.get_top().address())};
				return static_cast<Result &&>(*top.promise().ptr);
			}

			//! @returns awaiter for lazy increment
			//! @attention the returned awaiter must be awaited on on the coroutine that initially awaited @c generator::begin
			auto operator++() pre(handle and not handle.done()) { return iterator_awaiter<false>{*this}; }

			friend
			auto operator==(const iterator & self, std::default_sentinel_t) -> bool pre(self.handle) { return self.handle.done(); }
		private:
			friend
			generator;

			iterator(internal::unique_handle<promise_type> handle) noexcept : handle{std::move(handle)} {}

			internal::unique_handle<promise_type> handle;
		};

		template<bool Initial>
		class iterator_awaiter final : public internal::await_base {
			using value_type = std::conditional_t<Initial, iterator, iterator &>;

			value_type it;
			internal::nested_info n;
			std::coroutine_handle<> prev_top;
		public:
			iterator_awaiter(value_type it) pre(it.handle and not it.handle.done()) : it{std::forward<value_type>(it)} {}

			template<typename Promise>
			auto await_suspend(std::coroutine_handle<Promise> self) noexcept -> std::coroutine_handle<> {
				auto & it_promise{it.handle.promise()};
				contract_assert(not (it_promise.data & 1U));

				//! @attention connect @c it 's @c co_yield with current coroutine frame
				if constexpr(Initial) it_promise.set_continuation(self);
				else contract_assert(it_promise.get_continuation() == self);

				//! @attention store enough context to remove @c it from stack on resumption (as @c generator is not permanently on top of stack)
				prev_top = n.parent = self;

				//! @attention push @c it onto stack
				const auto & nested{self.promise().get_nested()};
				n.root = nested ? nested->root : std::addressof(self.promise());

				auto & rd{n.root->get_root()};
				rd.top = it_promise.get_top();
				it_promise.set_nested(n);

				if(rd.suspend()) return std::noop_coroutine();
				else return rd.top;
			}

			auto await_resume() {
				//! @note must be checked first, because if we got here via an unhandled exception, there is nothing to do aprdt from rethrowing
				if(n.eptr) std::rethrow_exception(n.eptr);

				auto & rd{n.root->get_root()};
				//! @attention @c it_promise.top won't be up to date, need to get actual top from @c *top so we can resume the correct coroutine on the next iteration
				it.handle.promise().set_top(rd.top);
				//! @attention pop @c it from stack by restoring the @c top we had on @c await_suspend
				rd.top = prev_top;

				if constexpr(Initial) return std::move(it);
			}
		};
	public:
		auto valueless() const noexcept -> bool { return not handle; }

		//! @returns awaiter for the initial iterator
		//! @attention transfers ownership of the managed coroutine to the resulting iterator
		//! @attention the returned iterator is bound to the calling coroutine
		auto begin() pre(not valueless()) post(valueless()) { return iterator_awaiter<true>{std::exchange(handle, {})}; }
		static
		auto end() noexcept -> std::default_sentinel_t { return std::default_sentinel; }
	private:
		friend
		internal::promise_base;
		friend
		root<generator>;

		generator(std::coroutine_handle<promise_type> handle) noexcept : handle{handle} {}

		internal::unique_handle<promise_type> handle;
	};

	//! @brief synchronization primitive
	class mutex final {
		using atomic_t = std::atomic<id>;
		static_assert(atomic_t::is_always_lock_free);
		atomic_t state;
	public:
		mutex() noexcept =default;
		mutex(const mutex &) =delete;
		auto operator=(const mutex &) -> mutex & =delete;
		~mutex() noexcept { if(state != id{}) std::terminate(); } //tried to destroy locked mutex

		//! @brief execute @c t whilst @c *this is locked
		template<typename Alloc, typename T>
		auto locked(std::allocator_arg_t, Alloc, task<T> t) -> task<T> pre(not t.valueless()) {
			const auto self{co_await get_identity};

			for(id expected{}; not state.compare_exchange_strong(expected, self); expected = {}) {
				if(expected == self) throw std::system_error{std::make_error_code(std::errc::resource_deadlock_would_occur)};
				co_yield blocked;
			}

			const struct guard final { atomic_t & state; ~guard() noexcept { state = id{}; } } g{state}; //defer...

			co_return co_await std::move(t);
		}

		template<typename T>
		auto locked(task<T> t) { return locked(std::allocator_arg, std::allocator<char>{}, std::move(t)); }
	};

	//! @brief synchronization primitive
	class shared_mutex final {
		using atomic_t = std::atomic<std::uint64_t>;
		static_assert(atomic_t::is_always_lock_free);
		atomic_t state;

		static
		constexpr
		std::uint64_t write_locked{std::uint64_t{1} << 63};
	public:
		shared_mutex() noexcept =default;
		shared_mutex(const shared_mutex &) =delete;
		auto operator=(const shared_mutex &) -> shared_mutex & =delete;
		~shared_mutex() noexcept { if(state != 0) std::terminate(); } //tried to destroy locked shared_mutex

		//! @brief execute @c t whilst @c *this is locked
		template<typename Alloc, typename T>
		auto locked(std::allocator_arg_t, Alloc, task<T> t) -> task<T> pre(not t.valueless()) {
			for(std::uint64_t expected{0}; not state.compare_exchange_strong(expected, write_locked); expected = 0) {
				//TODO: deadlock-detection like in @c mutex?
				co_yield blocked;
			}

			const struct guard final { atomic_t & state; ~guard() noexcept { state = 0; } } g{state}; //defer...

			co_return co_await std::move(t);
		}

		template<typename T>
		auto locked(task<T> t) { return locked(std::allocator_arg, std::allocator<char>{}, std::move(t)); }

		//! @brief execute @c t whilst @c *this is shared locked
		template<typename Alloc, typename T>
		auto shared_locked(std::allocator_arg_t, Alloc, task<T> t) -> task<T> pre(not t.valueless()) {
			for(auto val{state.load()};; val = state.load()) {
				if(val == write_locked) co_yield blocked;
				else {
					const auto new_{val + 1};
					if(new_ == write_locked) throw std::system_error{std::make_error_code(std::errc::value_too_large)};
					if(state.compare_exchange_strong(val, new_)) break;
				}
			}

			const struct guard final { atomic_t & state; ~guard() noexcept { --state; } } g{state}; //defer...

			co_return co_await std::move(t);
		}

		template<typename T>
		auto shared_locked(task<T> t) { return shared_locked(std::allocator_arg, std::allocator<char>{}, std::move(t)); }
	};

	template<template<typename> typename Wrapper, typename Result>
	struct [[nodiscard]] root<Wrapper<Result>> final {
		explicit
		root(Wrapper<Result> w) pre(not w.valueless()) : root{log_level::trace, std::move(w)} {}
		explicit
		root(log_level level, Wrapper<Result> w) pre(not w.valueless()) : ptr{std::make_unique<data>(level, std::move(w.handle))} {}

		auto valueless() const noexcept -> bool {
			if(ptr) {
				contract_assert(ptr->handle);
				return false;
			} else return true;
		}

		//! @returns the id of this coroutine stack, or a default-constructed id, if @c this is @c valueless
		auto get_id() const noexcept -> id {
			if(valueless()) return {};
			else return ptr->rd.get_id();
		}

		auto elapsed() const -> duration pre(not valueless()) { return ptr->timer.elapsed(); }

		auto log() const -> std::span<const log_message> pre(not valueless()) { return ptr->messages; }

		auto done() const -> bool pre(not valueless()) { return ptr->handle.done(); }

		auto wait() -> state pre(not valueless()) { return wait_with([]() noexcept { return false; }); }

		template<typename Rep, typename Period>
		auto wait_for(const std::chrono::duration<Rep, Period> & duration) -> state pre(not valueless()) { return wait_until(clock::now() + duration); }

		template<typename Clock, typename Duration>
		auto wait_until(const std::chrono::time_point<Clock, Duration> & time) -> state pre(not valueless()) {
			static_assert(compat::is_clock_v<Clock>);
			return wait_with([&]() noexcept { return Clock::now() >= time; });
		}

		template<typename Func>
		requires requires(const Func & f) { { f() } noexcept -> std::same_as<bool>; }
		auto wait_with(
			Func suspend //!< [in] execution suspends when @c suspend returns @c true @attention once @c suspend has returned @c true it may not return @c false again
		) -> state pre(not valueless()) {
			if(done()) return state::done;
			auto & rd{ptr->rd};
			rd.suspend = compat::function_ref<bool() const noexcept>{suspend};
			rd.reset_state();
			auto & timer{ptr->timer};
			timer.start();
			{
				const struct guard { timer_t & timer; ~guard() noexcept { timer.stop(); } } g{timer}; //defer...
				contract_assert(rd.top and not rd.top.done());
				rd.top.resume();
			}
			if(done()) return state::done;
			return rd.blocked() ? state::blocked : state::suspended;
		}

		//! @returns an empty @c optional if no result is available
		//! @attention the returned reference will dangle when the wrapped coroutine is resumed
		auto result() requires(not std::is_void_v<Result>) pre(not valueless()) {
			if(not ptr->rd.has_result()) return compat::optional_ref<Result>{};
			else {
				if constexpr(internal::task<Wrapper<Result>>) return compat::optional_ref<Result>(ptr->handle.promise().get_result());
				else {
					auto p{std::coroutine_handle<typename Wrapper<Result>::promise_type>::from_address(ptr->rd.top.address()).promise().ptr};
					contract_assert(p);
					return compat::optional_ref<Result>(*p);
				}
			}
		}
	private:
		class timer_t final {
			duration elapsed_{};
			std::optional<clock::time_point> last_resume; //set => coroutine stack is running...

			static
			auto now() noexcept -> clock::time_point { return clock::now(); }
		public:
			void start() post(last_resume) { last_resume = now(); }

			void stop() pre(last_resume) post(not last_resume) {
				elapsed_ += (now() - *last_resume);
				last_resume.reset();
			}

			auto elapsed() const noexcept -> duration {
				if(not last_resume) return elapsed_;
				else return elapsed_ + (now() - *last_resume);
			}
		};

		struct data final {
			using handle_t = internal::unique_handle<typename Wrapper<Result>::promise_type>;

			data(log_level level, handle_t h) pre(h and not h.done()) : level{level}, rd{*this}, handle{std::move(h)} {
				rd.top = handle;
				auto & p{handle.promise()};
				p.set_root(rd);
			}

			const log_level level;
			std::atomic<bool> message_lock{false};
			std::vector<log_message> messages;
			internal::root_data rd;
			handle_t handle;
			timer_t timer;
		};
		std::unique_ptr<data> ptr{std::make_unique<data>()};
	};
}

