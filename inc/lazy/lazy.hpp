
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

//TODO: root_generator?
//TODO: introduce id-type instead of using const void *?

//! @brief coroutine statements supported by all coroutine wrappers:
//!  * @code{.cpp} co_yield progress; @endcode to yield control back from the coroutine to the caller
//!  * @code{.cpp} co_await task; @endcode block this task until the awaited @c task is completed, then yield its result if any
//!  * @code{.cpp} co_await [fatal|error|warning|info|debug|trace]{fmt-string|args...}; @endcode create log of the respective severity
//!  * @code{.cpp} co_await get_identity; @endcode yields unique identification of coroutine stack
//!  * @code{.cpp} co_await timed{task}; @endcode block this task until the awaited @c task is completed, then yield the time it took to complete and its result if any
//!  * @code{.cpp} for co_await(<type> val : gen) { ... } @endcode block this task until awaited generator yields next value
//!  * @code{.cpp} co_return [val]; @endcode to terminate the task and optionally return a value to the caller
namespace lazy {
	using clock = std::chrono::steady_clock;
	using duration = clock::duration;

	template<typename>
	struct generator;

	template<typename>
	struct task;

	template<typename>
	struct root_task;

	enum class state {
		done,      //!< execution completed, result is ready
		suspended, //!< suspended due to timeout or request
		blocked,   //!< suspended due to synchronization primitive
	};

	enum class log_level { fatal, error, warning, info, debug, trace, };

	struct log_message final {
		std::source_location location;
		log_level level;
		std::string description;
	};

	//! @brief tag to time wall clock of execution of a @c task
	template<typename T>
	struct timed final { task<T> t; };

	//! @brief tag to yield all elements of a @c generator
	//! @note no support for general ranges, as those are not really compatible with lazy model
	template<typename T>
	struct elements_of final { generator<T> g; };

	namespace internal {
		template<typename T, typename... U>
		concept either = (std::same_as<T, U> or ...);

		template<typename T, typename... U>
		concept neither = not either<T, U...>;

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

		template<std::size_t Tag>
		struct tag_t final {
			constexpr
			explicit
			tag_t(int) noexcept {}
		};

		using progress_t = tag_t<0>;
		using get_identity_t = tag_t<1>;
		using set_blocked_t = tag_t<2>;

		struct awaiter_base {};

		struct root_data final {
			std::coroutine_handle<> top;

			//! @note inlined @code{.cpp} function_ref<bool() const noexcept> @endcode
			struct {
				void * ctx;
				bool (*fptr)(void *) noexcept;

				auto operator()() const noexcept -> bool { return fptr(ctx); }
			} suspend;

			//! @note @c true => we were suspended due to being blocked
			bool blocked; //TODO: optimize out ...

			class {
				template<typename>
				friend
				struct lazy::root_task;

				duration elapsed_{};
				std::optional<clock::time_point> last_resume; //set => coroutine stack is running...

				static
				auto now() noexcept -> clock::time_point { return clock::now(); }

				void start() /*TODO: [C++26] post(last_resume)*/ { last_resume = now(); }

				void stop() /*TODO: [C++26] pre(last_resume) post(not last_resume)*/ {
					elapsed_ += (now() - *last_resume);
					last_resume.reset();
				}
			public:
				auto elapsed() const noexcept -> duration {
					if(not last_resume) return elapsed_;
					else return elapsed_ + (now() - *last_resume);
				}
			} timer;

			struct {
				const log_level level;
				std::vector<log_message> messages; //TODO: allocators?
			} logging;
		};

		template<typename FmtArgs>
		struct [[nodiscard("must be awaited to take effect")]] log_message final {
			log_level level;
			std::string_view fmt;
			FmtArgs args;
		};

		class promise_base;

		struct nested_info final {
			std::exception_ptr eptr;        //needed for manual stack unwinding
			std::coroutine_handle<> parent; //directly preceding coroutine
			promise_base * root;            //bottom of implicit coroutine-"stack"
		};

		class promise_base {
			auto get_root() const -> const root_data & {
				auto nested{get_nested()};
				return *reinterpret_cast<const root_data *>(nested ? nested->root->data : data);
			}
			auto get_root() -> root_data & { return const_cast<root_data &>(static_cast<const promise_base *>(this)->get_root()); }
		public:
			//! @attention tagged "union"
			//! LSB set => nested_info *
			//! if promise is at bottom of coroutine-"stack" => @c root_data*
			//! else @c void* obtained from @c std::coroutine_handle<>::address of top-coroutine
			std::uintptr_t data;

			auto get_nested() const -> nested_info * { return (data & 1U) ? reinterpret_cast<nested_info *>(data ^ 1U) : nullptr; }
			void set_nested(nested_info & nested) /*TODO: [C++26] post(data & 1U)*/ { data = reinterpret_cast<std::uintptr_t>(&nested) | 1U; }

			//TODO: [C++26] generate get_return_object() with deducing this and reflection

			static
			auto initial_suspend() noexcept { return std::suspend_always{}; }
		private:
			struct pop_awaiter final : std::suspend_always {
				template<typename Promise>
				auto await_suspend(std::coroutine_handle<Promise> self) noexcept -> std::coroutine_handle<> {
					if(const auto nested{self.promise().get_nested()}) {
						auto rd{reinterpret_cast<root_data *>(nested->root->data)};
						rd->top = nested->parent;
						if(not rd->suspend()) return rd->top;
					}
					return std::noop_coroutine();
				}
			};
		public:
			static
			auto final_suspend() noexcept { return pop_awaiter{}; }

			void unhandled_exception(std::source_location loc = std::source_location::current()) {
				if(auto n{this->get_nested()}) n->eptr = std::current_exception();
				else {
					//! @note loc will not identify the actual throw-site, due to the coroutine body transformation
					auto & rd{*reinterpret_cast<root_data *>(data)};
					try { throw; }
					catch(const std::exception & exc) { rd.logging.messages.emplace_back(loc, log_level::fatal, exc.what()); }
					catch(...) { rd.logging.messages.emplace_back(loc, log_level::fatal, "unknown error"); }
					throw;
				}
			}

			auto await_transform(set_blocked_t) noexcept {
				get_root().blocked = true;
				return std::suspend_always{};
			}

			auto await_transform(get_identity_t) const noexcept {
				struct awaiter final : std::suspend_always {
					const root_data & rd;

					auto await_ready() const noexcept { return not rd.suspend(); }
					auto await_resume() const noexcept -> const void * { return std::addressof(rd); }
				};
				return awaiter{{}, get_root()};
			}

			auto yield_value(progress_t) const noexcept {
				struct awaiter final : std::suspend_always {
					const root_data & rd;

					auto await_ready() const noexcept { return not rd.suspend(); }
				};
				return awaiter{{}, get_root()};
			}

			template<typename FmtArgs>
			auto await_transform(log_message<FmtArgs> log, std::source_location loc = std::source_location::current()) {
				struct awaiter final : std::suspend_always {
					root_data & rd;
					log_message<FmtArgs> log;
					std::source_location loc;

					auto await_suspend(std::coroutine_handle<>) -> bool {
						if(rd.logging.level >= log.level) rd.logging.messages.emplace_back(loc, log.level, std::vformat(log.fmt, log.args));
						return rd.suspend();
					}
				};
				return awaiter{{}, get_root(), log, loc};
			}
		protected:
			template<typename T, bool Timed>
			class push_awaiter final : public std::suspend_always {
				nested_info n;
				unique_handle<T> other;
				//! @note only acecssed when @code{.cpp} Timed == true @endcode
				duration elapsed;
			public:
				push_awaiter(unique_handle<T> other) /*TODO: [C++26] pre(other and not other.done())*/ : other{std::move(other)} {}

				template<typename Promise>
				auto await_suspend(std::coroutine_handle<Promise> self) noexcept -> std::coroutine_handle<> {
					other.promise().set_nested(n);
					n.parent = self;
					auto nested{self.promise().get_nested()};
					n.root = nested ? nested->root : std::addressof(self.promise());
					auto rd{reinterpret_cast<root_data *>(n.root->data)};
					rd->top = other;
					if constexpr(Timed) elapsed = rd->timer.elapsed();
					if(rd->suspend()) return std::noop_coroutine();
					else return rd->top;
				}

				auto await_resume() /*TODO: [C++26] pre(other.done())*/ {
					if(n.eptr) std::rethrow_exception(n.eptr);
					if constexpr(Timed) {
						auto rd{reinterpret_cast<root_data *>(n.root->data)};
						elapsed = rd->timer.elapsed() - elapsed;
					}

					auto & promise{other.promise()};
					if constexpr(requires { promise.get_result(); }) { //! @note task
						using Result = decltype(promise.get_result());
						if constexpr(std::is_void_v<Result>) {
							if constexpr(Timed) return elapsed;
							else return;
						} else {
							if constexpr(Timed) return std::make_pair(elapsed, std::move(promise).get_result());
							else return std::move(promise).get_result();
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
			template<typename Self, typename T>
			requires std::same_as<Self, typename generator<T>::promise_type>
			auto yield_value(this Self & self, elements_of<T> other) /*TODO: [C++26] pre(not other.g.valueless() and not other.g.handle.promise().yield_target) pre(yield_target and not yield_target.done())*/ {
				other.g.handle.promise().yield_target = self.yield_target;
				return push<false>(std::move(other.g.handle));
			}

			template<typename T>
			static
			auto await_transform(task<T> other) /*TODO: [C++26] pre(not other.valueless())*/ { return push<false>(std::move(other.handle)); }

			template<typename T>
			static
			auto await_transform(timed<T> other) /*TODO: [C++26] pre(not other.task.valueless())*/ { return push<true>(std::move(other.task.handle)); }

			static
			auto await_transform(std::derived_from<awaiter_base> auto a) { return a; }
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
			std::optional<T> result;
		public:
			template<typename U = T>
			void return_value(U && value) noexcept { result.emplace(std::move(value)); }

			auto has_result() const noexcept -> bool { return result.has_value(); }

			auto get_result() & -> T & /*TODO: [C++26] pre(initialized)*/ { return *result; }
			auto get_result() && -> T && /*TODO: [C++26] pre(initialized)*/ { return std::move(*result); }
		};

		template<>
		class task_promise<void> : public promise_base {
			bool initialized{false};
		public:
			void return_void() noexcept { initialized = true; }

			auto has_result() const noexcept -> bool { return initialized; }

			void get_result() const /*TODO: [C++26] pre(initialized)*/ {}
		};
	}

	//! @brief tag to request identity of root of coroutine stack
	inline
	constexpr
	internal::get_identity_t get_identity{1};


	//! @brief tag to yield progress within a @c task
	//! @note not supported in @c generator to avoid ambiguity problems
	inline
	constexpr
	internal::progress_t progress{1};


	//TODO: documentation
	template<typename... Args>
	auto error(std::format_string<Args...> fmt, Args &&... args) { return internal::log_message{log_level::error, fmt.get(), std::make_format_args(args...)}; }
	//TODO: documentation
	template<typename... Args>
	auto warning(std::format_string<Args...> fmt, Args &&... args) { return internal::log_message{log_level::warning, fmt.get(), std::make_format_args(args...)}; }
	//TODO: documentation
	template<typename... Args>
	auto info(std::format_string<Args...> fmt, Args &&... args) { return internal::log_message{log_level::info, fmt.get(), std::make_format_args(args...)}; }
	//TODO: documentation
	template<typename... Args>
	auto debug(std::format_string<Args...> fmt, Args &&... args) { return internal::log_message{log_level::debug, fmt.get(), std::make_format_args(args...)}; }
	//TODO: documentation
	template<typename... Args>
	auto trace(std::format_string<Args...> fmt, Args &&... args) { return internal::log_message{log_level::trace, fmt.get(), std::make_format_args(args...)}; }


	//! @brief cooperative synchronous(!) recursive coroutine root task
	//! @tparam Result return type of the task
	template<typename Result = void>
	struct [[nodiscard]] root_task final {
		static_assert(std::is_void_v<Result> or (std::is_object_v<Result> and std::is_same_v<std::decay_t<Result>, Result>));

		struct promise_type final : internal::task_promise<Result> {
			internal::root_data root;

			promise_type() noexcept : promise_type{log_level::trace} {} //TODO: remove this ctor?

			promise_type(log_level level, auto &&... /*args*/) noexcept : root{.top = std::coroutine_handle<promise_type>::from_promise(*this), .logging{.level = level}} { //free function with no allocator
				this->data = reinterpret_cast<std::uintptr_t>(std::addressof(root));
			}
			promise_type(auto & /*this*/, log_level level, auto &&... /*args*/) noexcept : promise_type{level} {} //member function with no allocator
			promise_type(std::allocator_arg_t, auto & /*allocator*/, log_level level, auto &&... /*args*/) noexcept : promise_type{level} {} //free function with allocator
			promise_type(auto & /*this*/, std::allocator_arg_t, auto & /*allocator*/, log_level level, auto &&... /*args*/) noexcept : promise_type{level} {} //member function with allocator

			auto get_return_object() noexcept { return root_task{std::coroutine_handle<promise_type>::from_promise(*this)}; }
		};

		auto valueless() const noexcept -> bool { return not handle; }

		auto done() const -> bool /*TODO: [C++26] pre(not valueless())*/ { return handle.done(); }

		auto wait() -> state /*TODO: [C++26] pre(not valueless()) post(result: result == state::blocked or done())*/ { return wait_with([]() noexcept { return false; }); }

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
		auto wait_with(Func func) -> state /*TODO: [C++26] pre(not valueless())*/ { //TODO: replace Func with function_ref<bool() noexcept>
			static_assert(requires { { func() } noexcept -> std::same_as<bool>; });
			if(done()) return state::done;
			auto & promise{handle.promise()};
			auto & root{promise.root};
			root.suspend.ctx = std::addressof(func);
			root.suspend.fptr = +[](void * ptr) noexcept { return (*reinterpret_cast<Func *>(ptr))(); };
			root.blocked = false;
			root.timer.start();
			{
				const struct guard { internal::root_data & root; ~guard() noexcept { root.timer.stop(); } } g{root}; //defer...
				//TODO: [C++26] contract_assert(data.top and not data.top.done());
				promise.root.top.resume();
			}
			return done() ? state::done
			              : root.blocked ? state::blocked
			                             : state::suspended;
		}

		auto has_result() const -> bool /*TODO: [C++26] pre(done())*/ { return handle.promise().has_result(); }

		auto result() -> std::add_lvalue_reference_t<Result> /*TODO: [C++26] pre(has_result())*/ { return handle.promise().get_result(); }

		auto elapsed() const -> duration /*TODO: [C++26] pre(not valueless())*/ { return handle.promise().root.timer.elapsed(); }

		auto log() const -> std::span<const log_message> /*TODO: [C++26] pre(not valueless())*/ { return handle.promise().root.logging.messages; }
	private:
		root_task(std::coroutine_handle<promise_type> handle) noexcept : handle{handle} {}

		internal::unique_handle<promise_type> handle;
	};

	//! @brief cooperative synchronous(!) recursive coroutine task
	//! @tparam Result return type of the task
	template<typename Result = void>
	struct [[nodiscard]] task final {
		static_assert(std::is_void_v<Result> or (std::is_object_v<Result> and std::is_same_v<std::decay_t<Result>, Result>));

		struct promise_type final : internal::task_promise<Result> {
			promise_type() { this->data = reinterpret_cast<std::uintptr_t>(std::coroutine_handle<promise_type>::from_promise(*this).address()); }

			auto get_return_object() noexcept { return task{std::coroutine_handle<promise_type>::from_promise(*this)}; }
		};

		auto valueless() const noexcept -> bool { return not handle; }
	private:
		friend
		internal::promise_base;

		task(std::coroutine_handle<promise_type> handle) noexcept : handle{handle} {}

		internal::unique_handle<promise_type> handle;
	};

	//! @brief cooperative synchronous(!) recursive coroutine generator
	//! @tparam Result return type of the generator
	//! additional supported coroutine statements:
	//!  * @code{.cpp} co_yield val; @endcode yield value to caller of generator
	//!  * @code{.cpp} co_yield elements_of{generator}; @endcode yield elements of @c generator
	template<typename Result>
	struct [[nodiscard]] generator final {
		static_assert(std::is_object_v<Result> and std::is_same_v<std::decay_t<Result>, Result>);

		struct promise_type final : internal::promise_base {
			Result * ptr;
			std::coroutine_handle<> yield_target;

			promise_type() { this->data = reinterpret_cast<std::uintptr_t>(std::coroutine_handle<promise_type>::from_promise(*this).address()); }

			auto get_return_object() noexcept { return generator{std::coroutine_handle<promise_type>::from_promise(*this)}; }

			using internal::promise_base::yield_value;

			auto yield_value(const Result & lval) requires std::is_copy_constructible_v<Result> /*TODO: [C++26] pre(yield_target and not yield_target.done())*/ {
				struct awaiter final : std::suspend_always {
					Result val;

					//! @note does not check for suspension, as we need to jump back to @c yield_target
					auto await_suspend(std::coroutine_handle<promise_type> self) noexcept {
						self.promise().ptr = std::addressof(val);
						return self.promise().yield_target;
					}
				};
				return awaiter{{}, lval};
			}

			auto yield_value(Result && val) noexcept /*TODO: [C++26] pre(yield_target and not yield_target.done())*/ {
				ptr = std::addressof(val);
				struct awaiter final : std::suspend_always {
					//! @note does not check for suspension, as we need to jump back to @c yield_target
					auto await_suspend(std::coroutine_handle<promise_type> self) const noexcept { return self.promise().yield_target; }
				};
				return awaiter{};
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
				//TODO: [C++26] contract_assert(not (promise.data & 1U));
				auto top{std::coroutine_handle<promise_type>::from_address(reinterpret_cast<void *>(promise.data))};
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
		class iterator_awaiter final : public std::suspend_always, public internal::awaiter_base {
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
				if constexpr(Initial) it_promise.yield_target = self;
				//TODO: [C++26] else contract_assert(it_promise.yield_target == self);

				//! @attention store enough context to remove @c it from stack on resumption (as @c generator is not permanently on top of stack)
				prev_top = n.parent = self;

				//! @attention push @c it onto stack
				const auto & nested{self.promise().get_nested()};
				n.root = nested ? nested->root : std::addressof(self.promise());

				auto rd{reinterpret_cast<internal::root_data *>(n.root->data)};
				rd->top = std::coroutine_handle<>::from_address(reinterpret_cast<void *>(it_promise.data));
				it_promise.set_nested(n);

				if(rd->suspend()) return std::noop_coroutine();
				else return rd->top;
			}

			auto await_resume() {
				//! @note must be checked first, because if we got here via an unhandled exception, there is nothing to do aprdt from rethrowing
				if(n.eptr) std::rethrow_exception(n.eptr);

				auto rd{reinterpret_cast<internal::root_data *>(n.root->data)};
				//! @attention @c it_promise.top won't be up to date, need to get actual top from @c *top so we can resume the correct coroutine on the next iteration
				it.handle.promise().data = reinterpret_cast<std::uintptr_t>(rd->top.address());
				//! @attention pop @c it from stack by restoring the @c top we had on @c await_suspend
				rd->top = prev_top;

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

		generator(std::coroutine_handle<promise_type> handle) noexcept : handle{handle} {}

		internal::unique_handle<promise_type> handle;
	};


	//! @brief synchronization primitive
	class mutex final {
		std::atomic<const void *> state{nullptr};
	public:
		mutex() noexcept =default;
		mutex(const mutex &) =delete;
		auto operator=(const mutex &) -> mutex & =delete;
		~mutex() noexcept { if(state) std::terminate(); } //tried to destroy locked mutex

		//! @brief execute @c t whilst @c *this is locked
		template<typename T>
		auto locked(task<T> t) -> task<T> /*TODO: [C++26] pre(not t.valueless())*/ {
			const auto id{co_await get_identity};

			for(const void * ptr{nullptr}; not state.compare_exchange_strong(ptr, id); ptr = nullptr) {
				if(ptr == id) throw std::system_error{std::make_error_code(std::errc::resource_deadlock_would_occur)};
				co_await internal::set_blocked_t{1};
			}

			const struct guard final {
				mutex & m;

				~guard() noexcept { m.state.store(nullptr); }
			} g{*this};

			co_return co_await std::move(t);
		}
	};
}

