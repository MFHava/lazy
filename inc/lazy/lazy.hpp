
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
#include <cstdint>
#include <cstring>
#include <utility>
#include <optional>
#include <coroutine>
#include <type_traits>
#include <system_error>
#include <source_location>

//TODO: solution for supporting fork-join pattern?
//TODO: TLS replacement?

//! @brief coroutine statements supported by all coroutine wrappers:
//!  * @code{.cpp} co_yield progress; @endcode to yield control back from the coroutine to the caller
//!  * @code{.cpp} co_await task; @endcode block current coroutine until the awaited @c task is completed, then returns its result if any
//!  * @code{.cpp} co_await [error|warning|info|debug]{fmt-string, args...}; @endcode create log of the respective severity
//!  * @code{.cpp} co_await <dump>; @endcode where @c <dump> is derived from @c dump_base create dump entry if coroutine stack is executing with @c log_level::trace
//!  * @code{.cpp} co_await get_identity; @endcode yields unique identification of coroutine stack
//!  * @code{.cpp} co_await get_is_tracing; @endcode yields @c true if coroutine stack is executing with @c log_level::trace
//!  * @code{.cpp} co_await timed{task}; @endcode block this coroutine until the awaited @c task is completed, then returns the time it took to complete and its result if any
//!  * @code{.cpp} for co_await(<type> val : gen) { ... } @endcode block this task until awaited generator yields next value
namespace lazy {
	using clock = std::chrono::steady_clock;
	using duration = clock::duration;

	template<typename>
	struct generator;

	template<typename>
	struct task;

	template<typename>
	struct root;

	//TODO: documentation
	enum class state {
		done,      //!< execution completed, @c task result may be ready
		suspended, //!< suspended due to timeout or user request, @c generator result may be ready
		blocked,   //!< suspended due to synchronization primitive
	};

	//TODO: documentation
	enum class log_level {
		fatal,
		error,
		warning,
		info,
		debug,
		trace,
	};

	//TODO: documentation
	struct log_message final {
		std::source_location location;
		log_level level;
		//! @brief if @code{.cpp} level == log_level::trace @endcode, then @c data is: @c [filename\0content], else it is the log mesage
		std::string data;
	};

	//! @brief tag to time wall clock of execution of a @c task
	template<typename T>
	struct timed final { task<T> t; };

	//! @brief tag to yield all elements of a @c generator
	//! @note no support for general ranges, as those are not really compatible with lazy model
	template<typename T>
	struct elements_of final { generator<T> g; };

	namespace internal {
		struct root_data;
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

			auto done() const /*TODO: [C++26] pre(handle)*/ { return handle.done(); }
			void resume() /*TODO: [C++26] pre(not done())*/ { handle.resume(); }
			auto promise() const -> Promise & /*TODO: [C++26] pre(handle)*/ { return handle.promise(); }

			explicit
			operator bool() const noexcept { return static_cast<bool>(handle); }
			operator std::coroutine_handle<>() const noexcept { return handle; }
		};

		//! @brief base for all awaiters that will be passed through @c await_transform transparently
		struct await_base : std::suspend_always {};

		//! @brief base for all awaiters that will be passed through @c yield_value transparently
		struct yield_base : std::suspend_always {};

		struct root_data final {
			auto get_id() const noexcept { return id{reinterpret_cast<std::uintptr_t>(static_cast<const void *>(this))}; }

			std::coroutine_handle<> top;

			//TODO: increase encapsulation

			//! @note inlined @code{.cpp} function_ref<bool() const noexcept> @endcode
			struct {
				void * ctx;
				bool (*fptr)(void *) noexcept;

				auto operator()() const noexcept -> bool { return fptr(ctx); }
			} suspend;

			class {
				duration elapsed_{};
				std::optional<clock::time_point> last_resume; //set => coroutine stack is running...

				static
				auto now() noexcept -> clock::time_point { return clock::now(); }
			public:
				void start() /*TODO: [C++26] post(last_resume)*/ { last_resume = now(); }

				void stop() /*TODO: [C++26] pre(last_resume) post(not last_resume)*/ {
					elapsed_ += (now() - *last_resume);
					last_resume.reset();
				}

				auto elapsed() const noexcept -> duration {
					if(not last_resume) return elapsed_;
					else return elapsed_ + (now() - *last_resume);
				}
			} timer;

			struct {
				const log_level level;
				std::vector<log_message> messages; //TODO: allocators?
			} logging;

			class {
				//! @attention tagged "union"
				//! LSB set => suspended due to being blocked
				//! else: for root<task<?>>: UNUSED
				//! else: for root<generator<?>>:
				//!   if equal to 0 => suspended without value
				//!   else => pointer to last yield-result
				std::uintptr_t data;
			public:
				void reset() noexcept { data = 0; }

				void set_yield_result(void * ptr) /*TODO: [C++26] pre(data == 0)*/ { data = reinterpret_cast<std::uintptr_t>(ptr); }
				void set_blocked() /*TODO: [C++26] pre(data == 0)*/ { data = 1U; }

				auto yield_result() const noexcept -> void * {
					if(blocked()) return nullptr;
					return reinterpret_cast<void *>(data);
				}
				auto blocked() const noexcept -> bool { return data & 1U; }
			} suspension_state;
		};

		struct promise_base;

		struct nested_info final {
			std::exception_ptr eptr;        //needed for manual stack unwinding
			std::coroutine_handle<> parent; //directly preceding coroutine
			promise_base * root;            //bottom of implicit coroutine-"stack"
		};

		struct promise_base {
			class {
				//! @attention tagged "union"
				//! LSB set => nested_info *
				//! LSB + 1 set => root_data * (=> this promise is at bottom of "stack")
				//! else @c void* obtained from @c std::coroutine_handle<>::address of top-coroutine
				std::uintptr_t data;
			public:
				void set_nested(nested_info & nested) { data = reinterpret_cast<std::uintptr_t>(&nested) | 1U; }
				void set_top(std::coroutine_handle<> handle) { data = reinterpret_cast<std::uintptr_t>(handle.address()); }
				void set_root(root_data & rd) { data = reinterpret_cast<std::uintptr_t>(std::addressof(rd)) | 2U; }

				auto get_nested() const -> nested_info * /*TODO: [C++26] pre(not (data & 2U))*/ { return (data & 1U) ? reinterpret_cast<nested_info *>(data ^ 1U) : nullptr; }
				auto get_top() const -> std::coroutine_handle<> /*TODO: [C++26] pre(not (data & 3U))*/ { return std::coroutine_handle<>::from_address(reinterpret_cast<void *>(data)); }
				auto get_root() const -> root_data & /*TODO: [C++26] pre(not (data & 1U) and (data & 2U))*/ { return *reinterpret_cast<root_data *>(data ^ 2U); }

				auto find_root() const -> root_data & {
					if(auto nested{get_nested()}) return nested->root->data.get_root();
					else return get_root();
				}
			} data;

			//TODO: [C++26] generate get_return_object() with deducing this and reflection

			static
			auto initial_suspend() noexcept { return std::suspend_always{}; }
		private:
			struct pop_awaiter final : std::suspend_always {
				template<typename Promise>
				auto await_suspend(std::coroutine_handle<Promise> self) noexcept -> std::coroutine_handle<> {
					if(const auto nested{self.promise().data.get_nested()}) {
						auto & rd{nested->root->data.get_root()};
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
				if(auto n{data.get_nested()}) n->eptr = std::current_exception();
				else {
					//! @note loc will not identify the actual throw-site, due to the coroutine body transformation
					auto & rd{data.get_root()};
					try { throw; }
					catch(const std::exception & exc) { rd.logging.messages.emplace_back(loc, log_level::fatal, exc.what()); }
					catch(...) { rd.logging.messages.emplace_back(loc, log_level::fatal, "unknown error"); }
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
				push_awaiter(unique_handle<T> other) /*TODO: [C++26] pre(other and not other.done())*/ : other{std::move(other)} {}

				template<typename Promise>
				auto await_suspend(std::coroutine_handle<Promise> self) noexcept -> std::coroutine_handle<> {
					other.promise().data.set_nested(n);
					n.parent = self;
					auto nested{self.promise().data.get_nested()};
					n.root = nested ? nested->root : std::addressof(self.promise());
					auto & rd{n.root->data.get_root()};
					rd.top = other;
					if constexpr(Timed) elapsed = rd.timer.elapsed();
					if(rd.suspend()) return std::noop_coroutine();
					else return rd.top;
				}

				auto await_resume() /*TODO: [C++26] pre(other.done())*/ {
					if(n.eptr) std::rethrow_exception(n.eptr);
					if constexpr(Timed) {
						auto & rd{n.root->data.get_root()};
						elapsed = rd.timer.elapsed() - elapsed;
					}

					auto & promise{other.promise()};
					if constexpr(requires { promise.get_result(); }) { //! @note task
						using Result = decltype(promise.get_result());
						if constexpr(std::is_void_v<Result>) {
							if constexpr(Timed) return elapsed;
							else return;
						} else {
							if constexpr(Timed) return std::make_pair(elapsed, std::move(promise.get_result()));
							else return std::move(promise.get_result());
						}
					} else { //! @note generator
						static_assert(not Timed);
						return;
					}
				}
			};

			template<bool Timed, typename T>
			static
			auto push(unique_handle<T> handle) /*TODO: [C++26] pre(handle and not handle.done())*/ { return push_awaiter<T, Timed>{std::move(handle)}; }
		public:
			template<typename T>
			static
			auto await_transform(task<T> other) /*TODO: [C++26] pre(not other.valueless())*/ { return push<false>(std::move(other.handle)); }

			template<typename T>
			static
			auto await_transform(timed<T> other) /*TODO: [C++26] pre(not other.task.valueless())*/ { return push<true>(std::move(other.task.handle)); }

			static
			auto await_transform(std::derived_from<await_base> auto a) noexcept { return a; }

			template<typename Self, typename T>
			requires std::same_as<Self, typename generator<T>::promise_type>
			auto yield_value(this Self & self, elements_of<T> other) /*TODO: [C++26] pre(not other.g.valueless() and not other.g.handle.promise().yield_target)*/ {
				other.g.handle.promise().yield_target = self.yield_target;
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
				//TODO: [C++26] contract_assert(alloc);
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
				//TODO: [C++26] contract_assert(d);
				reinterpret_cast<deleter_t>(d)(static_cast<std::byte *>(ptr), size);
			}
		};

		template<typename T>
		class task_promise : public promise_base {
			union { T result; };
			bool initialized{false};
		public:
			bool update_suspension_state{false};

			task_promise() noexcept {}
			task_promise(const task_promise &) =delete;
			auto operator=(const task_promise &) -> task_promise & =delete;
			~task_promise() noexcept { if(initialized) result.~T(); }

			template<typename U = T>
			void return_value(U && value) {
				new(std::addressof(result)) T(std::forward<U>(value));
				initialized = true;
				if(update_suspension_state) this->data.get_root().suspension_state.set_yield_result(std::addressof(result));
			}

			auto get_result() -> T & /*TODO: [C++26] pre(initialized)*/ { return result; }
		};

		template<>
		struct task_promise<void> : promise_base {
			static
			void return_void() noexcept {}

			static
			void get_result() noexcept {}
		};

		struct progress_t final : yield_base {
			template<typename Promise>
			auto await_suspend(std::coroutine_handle<Promise> self) const noexcept { return self.promise().data.find_root().suspend(); }
		};

		struct blocked_t final : yield_base { //TODO: add awaiter to public API?
			template<typename Promise>
			void await_suspend(std::coroutine_handle<Promise> self) const noexcept { self.promise().data.find_root().suspension_state.set_blocked(); }
		};

		class get_identity_t final : public await_base {
			id result;
		public:
			template<typename Promise>
			auto await_suspend(std::coroutine_handle<Promise> self) noexcept -> bool {
				const auto & rd{self.promise().data.find_root()};
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
				const auto & rd{self.promise().data.find_root()};
				result = rd.logging.level == log_level::trace;
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
				auto & rd{self.promise().data.find_root()};
				if(rd.logging.level >= level) rd.logging.messages.emplace_back(loc, level, std::vformat(fmt, args));
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

	//! @brief awaiter to yield progress within a @c task or @c generator
	inline
	constexpr
	internal::progress_t progress;


	//TODO: documentation
	template<typename... Args>
	struct [[nodiscard("must be awaited to take effect")]] error final : internal::log_message<Args...> {
		error(std::format_string<Args...> fmt, Args &&... args, std::source_location loc = std::source_location::current()) noexcept : internal::log_message<Args...>{log_level::error, fmt, std::forward<Args>(args)..., loc} {}
	};
	template<typename... Args>
	error(std::format_string<Args...>, Args &&...) -> error<Args...>;
	//TODO: documentation
	template<typename... Args>
	struct [[nodiscard("must be awaited to take effect")]] warning final : internal::log_message<Args...> {
		warning(std::format_string<Args...> fmt, Args &&... args, std::source_location loc = std::source_location::current()) noexcept : internal::log_message<Args...>{log_level::warning, fmt, std::forward<Args>(args)..., loc} {}
	};
	template<typename... Args>
	warning(std::format_string<Args...>, Args &&...) -> warning<Args...>;
	//TODO: documentation
	template<typename... Args>
	struct [[nodiscard("must be awaited to take effect")]] info final : internal::log_message<Args...> {
		info(std::format_string<Args...> fmt, Args &&... args, std::source_location loc = std::source_location::current()) noexcept : internal::log_message<Args...>{log_level::info, fmt, std::forward<Args>(args)..., loc} {}
	};
	template<typename... Args>
	info(std::format_string<Args...>, Args &&...) -> info<Args...>;
	//TODO: documentation
	template<typename... Args>
	struct [[nodiscard("must be awaited to take effect")]] debug final : internal::log_message<Args...> {
		debug(std::format_string<Args...> fmt, Args &&... args, std::source_location loc = std::source_location::current()) noexcept : internal::log_message<Args...>{log_level::debug, fmt, std::forward<Args>(args)..., loc} {}
	};
	template<typename... Args>
	debug(std::format_string<Args...>, Args &&...) -> debug<Args...>;

	//TODO: documentation
	class dump_base : public internal::await_base {
		//TODO: documentation
		virtual
		void dump_to(std::back_insert_iterator<std::string> result) const =0;

		std::string_view file_name;
		std::source_location loc;
	public:
		dump_base(std::string_view file_name, std::source_location loc) noexcept : file_name{file_name}, loc{loc} {}

		template<typename Promise>
		auto await_suspend(std::coroutine_handle<Promise> self) const {
			auto & rd{self.promise().find_root()};
			if(rd.level == log_level::trace) {
				std::string msg{file_name};
				msg += '\0';
				dump_to(std::back_inserter(msg));
				rd.logging.messages.emplace_back(loc, log_level::trace, std::move(msg));
			}
			return rd.suspend();
		}
	};

	//! @brief cooperative synchronous(!) recursive coroutine task
	//! @tparam Result return type of the task
	//! additional supported coroutine statements:
	//!  * @code{.cpp} co_return [val]; @endcode to terminate the task and optionally return a value to the caller
	template<typename Result = void>
	struct [[nodiscard]] task final {
		static_assert(std::is_void_v<Result> or (std::is_object_v<Result> and std::is_same_v<std::decay_t<Result>, Result>));

		struct promise_type final : internal::task_promise<Result> {
			promise_type() { this->data.set_top(std::coroutine_handle<promise_type>::from_promise(*this)); }

			auto get_return_object() noexcept { return task{std::coroutine_handle<promise_type>::from_promise(*this)}; }
		};

		auto valueless() const noexcept -> bool { return not handle; }
	private:
		friend
		internal::promise_base;
		friend
		root<task>;

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

		struct promise_type final : internal::promise_base {
			Result * ptr;
			class {
				//! @attention tagged "union"
				//! LSB set => yielding to @c root<generator> => must update @c root_data.ptr and suspend
				//! else: address of coroutine_handle to yield to (must update @c ptr)
				std::uintptr_t data{0};
			public:
				void set_update_suspension_state() /*TODO: [C++26] pre(data == 0))*/ { data = 1U; }
				void set_continuation(std::coroutine_handle<> handle) /*TODO: [C++26] pre(data == 0)*/ { data = reinterpret_cast<std::uintptr_t>(handle.address()); }

				auto get() const -> std::coroutine_handle<> /*TODO: [C++26] pre(data != 0)*/ {
					if(data & 1U) return {};
					return std::coroutine_handle<>::from_address(reinterpret_cast<void *>(data));
				}
			} yield_target;

			promise_type() { this->data.set_top(std::coroutine_handle<promise_type>::from_promise(*this)); }

			auto get_return_object() noexcept { return generator{std::coroutine_handle<promise_type>::from_promise(*this)}; }

			using internal::promise_base::yield_value;

			auto yield_value(const Result & lval) requires std::is_copy_constructible_v<Result> {
				struct awaiter final : std::suspend_always {
					Result val;

					//! @note does not check for suspension, as we need to jump back to @c yield_target
					auto await_suspend(std::coroutine_handle<promise_type> self) noexcept -> std::coroutine_handle<> {
						auto & promise{self.promise()};
						if(const auto cont{promise.yield_target.get()}) {
							//TODO: [C++26] contract_assert(not cont.done());
							promise.ptr = std::addressof(val);
							return cont;
						} else {
							promise.data.find_root().suspension_state.set_yield_result(std::addressof(val));
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

				if(const auto cont{yield_target.get()}) {
					//TODO: [C++26] contract_assert(not cont.done());
					ptr = std::addressof(val);
					return awaiter{{}, cont};
				} else {
					data.find_root().suspension_state.set_yield_result(std::addressof(val));
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

			auto operator*() const -> Result && /*TODO: [C++26] pre(handle and not handle.done())*/ {
				auto & promise{handle.promise()};
				auto top{std::coroutine_handle<promise_type>::from_address(promise.data.get_top().address())};
				return static_cast<Result &&>(*top.promise().ptr);
			}

			//! @returns awaiter for lazy increment
			//! @attention the returned awaiter must be awaited on on the coroutine that initially awaited @c generator::begin
			auto operator++() /*TODO: [C++26] pre(handle and not handle.done())*/ { return iterator_awaiter<false>{*this}; }

			friend
			auto operator==(const iterator & self, std::default_sentinel_t) -> bool /*TODO: [C++26] pre(self.handle)*/ { return self.handle.done(); }
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
			iterator_awaiter(value_type it) /*TODO: [C++26] pre(it.handle and not it.handle.done())*/ : it{std::forward<value_type>(it)} {}

			template<typename Promise>
			auto await_suspend(std::coroutine_handle<Promise> self) noexcept -> std::coroutine_handle<> {
				auto & it_promise{it.handle.promise()};
				//TODO: [C++26] contract_assert(not (it_promise.data & 1U));

				//! @attention connect @c it 's @c co_yield with current coroutine frame
				if constexpr(Initial) it_promise.yield_target.set_continuation(self);
				//TODO: [C++26] else contract_assert(it_promise.yield_target == self);

				//! @attention store enough context to remove @c it from stack on resumption (as @c generator is not permanently on top of stack)
				prev_top = n.parent = self;

				//! @attention push @c it onto stack
				const auto & nested{self.promise().data.get_nested()};
				n.root = nested ? nested->root : std::addressof(self.promise());

				auto & rd{n.root->data.get_root()};
				rd.top = it_promise.data.get_top();
				it_promise.data.set_nested(n);

				if(rd.suspend()) return std::noop_coroutine();
				else return rd.top;
			}

			auto await_resume() {
				//! @note must be checked first, because if we got here via an unhandled exception, there is nothing to do aprdt from rethrowing
				if(n.eptr) std::rethrow_exception(n.eptr);

				auto & rd{n.root->data.get_root()};
				//! @attention @c it_promise.top won't be up to date, need to get actual top from @c *top so we can resume the correct coroutine on the next iteration
				it.handle.promise().data.set_top(rd.top);
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
		auto begin() /*TODO: [C++26] pre(not valueless()) post(valueless())*/ { return iterator_awaiter<true>{std::exchange(handle, {})}; }
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
		template<typename T>
		auto locked(task<T> t) -> task<T> /*TODO: [C++26] pre(not t.valueless())*/ {
			const auto self{co_await get_identity};

			for(id expected{}; not state.compare_exchange_strong(expected, self); expected = {}) {
				if(expected == self) throw std::system_error{std::make_error_code(std::errc::resource_deadlock_would_occur)};
				co_yield internal::blocked_t{};
			}

			const struct guard final { mutex & m; ~guard() noexcept { m.state.store({}); } } g{*this}; //defer...

			co_return co_await std::move(t);
		}
	};

	//! @brief root of coroutine stack
	//! @tparam Wrapper type of wrapper that is managed
	template<typename Wrapper>
	struct root;

	template<template<typename> typename Wrapper, typename Result>
	struct [[nodiscard]] root<Wrapper<Result>> final {
		//TODO: [[deprecated]]
		root(Wrapper<Result> w) /*TODO: [C++26] pre(not w.valueless())*/ : root{log_level::trace, std::move(w)} {}

		root(log_level level, Wrapper<Result> w) /*TODO: [C++26] pre(not w.valueless())*/ : ptr{std::make_unique<data>(level, std::move(w.handle))} {}

		auto valueless() const noexcept -> bool {
			if(ptr) return false;
			else {
				//TODO: [C++26] contract_assert(ptr->handle);
				return true;
			}
		}

		//! @returns the id of this coroutine stack, or a default-constructed id, if @c this is @c valueless
		auto get_id() const noexcept -> id {
			if(valueless()) return {};
			else return ptr->root.get_id();
		}

		auto elapsed() const -> duration /*TODO: [C++26] pre(not valueless())*/ { return ptr->root.timer.elapsed(); }

		auto log() const -> std::span<const log_message> /*TODO: [C++26] pre(not valueless())*/ { return ptr->root.logging.messages; }

		auto done() const -> bool /*TODO: [C++26] pre(not valueless())*/ { return ptr->handle.done(); }

		auto wait() -> state /*TODO: [C++26] pre(not valueless())*/ { return wait_with([]() noexcept { return false; }); }

		template<typename Rep, typename Period>
		auto wait_for(const std::chrono::duration<Rep, Period> & duration) -> state /*TODO: [C++26] pre(not valueless())*/ { return wait_until(clock::now() + duration); }

		template<typename Clock, typename Duration>
		auto wait_until(const std::chrono::time_point<Clock, Duration> & time) -> state /*TODO: [C++26] pre(not valueless())*/ {
#if __cpp_lib_chrono >= 201907L
			static_assert(std::chrono::is_clock_v<Clock>);
#endif
			return wait_with([&]() noexcept { return Clock::now() >= time; });
		}

		template<typename Func>
		auto wait_with(Func func) -> state /*TODO: [C++26] pre(not valueless())*/ {
			static_assert(requires { { func() } noexcept -> std::same_as<bool>; });
			if(done()) return state::done;
			auto & rd{ptr->root};
			rd.suspend.ctx = std::addressof(func);
			rd.suspend.fptr = +[](void * ptr) noexcept { return (*reinterpret_cast<Func *>(ptr))(); };
			rd.suspension_state.reset();
			rd.timer.start();
			{
				const struct guard { internal::root_data & rd; ~guard() noexcept { rd.timer.stop(); } } g{rd}; //defer...
				//TODO: [C++26] contract_assert(data.top and not data.top.done());
				rd.top.resume();
			}
			if(done()) return state::done;
			return rd.suspension_state.blocked() ? state::blocked : state::suspended;
		}

		//TODO: [C++26] remove has_result and have result return optional<result_type &>

		auto has_result() const -> bool requires(not std::is_void_v<Result>)/*TODO: [C++26] pre(not valueless())*/ { return ptr->root.suspension_state.yield_result() != nullptr; }

		auto result() -> std::add_lvalue_reference_t<Result> requires(not std::is_void_v<Result>) /*TODO: [C++26] pre(has_result())*/ { return *reinterpret_cast<Result *>(ptr->root.suspension_state.yield_result()); }
	private:
		struct data final {
			using handle_t = internal::unique_handle<typename Wrapper<Result>::promise_type>;

			data(log_level level, handle_t h) /*TODO: [C++26] pre(not w.valueless())*/ : root{.top = h, .logging = {.level = level}}, handle{std::move(h)} {
				auto & p{handle.promise()};
				p.data.set_root(root);
				if constexpr(requires{ p.yield_target; }) p.yield_target.set_update_suspension_state(); //when wrapping a generator<T>, it should yield to the storage in root_data, not it's internal pointer
				if constexpr(requires { p.update_suspension_state; }) p.update_suspension_state = true; //when wrapping a task<T>, where T != void, it should yield to the storeage in root_data
			}

			internal::root_data root;
			handle_t handle;
		};
		std::unique_ptr<data> ptr{std::make_unique<data>()};
	};

	template<typename Wrapper>
	root(Wrapper) -> root<Wrapper>;
	template<typename Wrapper>
	root(log_level, Wrapper) -> root<Wrapper>;
}

