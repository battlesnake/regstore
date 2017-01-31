#pragma once
/* Register store, supporting read/write-only dynamic registers and observers */
#include <cstd/std.hpp>

namespace mark {

/*
 * Observers are called from within a lock so do not call any methods on the
 * regstore from within an observer
 */
class regstore {
public:
	enum err {
		ok,
		invalid_key,
		invalid_value,
		unknown,
		not_readable,
		not_writeable
	};
	static const char *errstr(err error);
	using getter = std::function<err(std::string&)>;
	using setter = std::function<err(const std::string&)>;
	using observer = std::function<void(const std::string& value)>;
	using reg_type = int;
	static constexpr reg_type rt_none = 0, rt_readable = 1, rt_writeable = 2;
	struct subscription_info {
		std::chrono::steady_clock::time_point next;
		std::chrono::steady_clock::duration min_interval;
	};
	struct register_info {
		reg_type type = rt_none;
		bool subscribed = false;
		subscription_info sub_info;
	};
	using register_list = std::unordered_map<std::string, register_info>;
private:
	struct observer_entry : subscription_info {
		observer func;
	};
	mutable std::mutex mx;
	/* Register name, getter/setter */
	std::unordered_map<std::string, std::pair<getter, setter>> store;
	/* Register name, remote name, observer */
	std::unordered_map<std::string, std::unordered_map<std::string, observer_entry>> observers;

	register_list _list(const std::string& remote) const;
	void _add(const std::string& key, const getter& get, const setter& set);
	err _set(const std::string& key, const std::string& value);
	err _get(const std::string& key, std::string& value) const;
	void _observe(const std::string& key, const std::string& remote, const observer& obs, const std::chrono::steady_clock::duration& min_interval);
	void _send_notification(const std::string& key, const std::string& value) const;
	void _unobserve(const std::string& key, const std::string& remote);
	bool _query_observer(const std::string& key, const std::string& remote, subscription_info& info) const;
	err _notify(const std::string& key) const;
	template <typename... T>
	void _notify(const std::string& key, const T&... keys) const
		{ _notify(key); _notify(std::forward<const T&>(keys)...); }

public:

	register_list list(const std::string& remote = "")
		{ std::lock_guard<std::mutex> lock(mx); return _list(remote); }

	void add(const std::string& key, getter get, setter set)
		{ std::lock_guard<std::mutex> lock(mx); _add(key, get, set); }

	err set(const std::string& key, const std::string& value)
		{ std::lock_guard<std::mutex> lock(mx); return _set(key, value); }

	err get(const std::string& key, std::string& value) const
		{ std::lock_guard<std::mutex> lock(mx); return _get(key, value); }

	template <typename Rep, typename Period>
	void observe(const std::string& key, const std::string& remote, const observer& obs, const std::chrono::duration<Rep, Period>& min_interval)
		{ std::lock_guard<std::mutex> lock(mx); _observe(key, remote, obs, std::chrono::duration_cast<std::chrono::steady_clock::duration>(min_interval)); }

	void unobserve(const std::string& key, const std::string& remote)
		{ std::lock_guard<std::mutex> lock(mx); _unobserve(key, remote); }

	bool query_observer(const std::string& key, const std::string& remote, subscription_info& info) const
		{ return _query_observer(key, remote, info); }

	err notify(const std::string& key) const
		{ std::lock_guard<std::mutex> lock(mx); return _notify(key); }

	template <typename... T>
	void notify(const T&... keys)
		{ std::lock_guard<std::mutex> lock(mx); _notify(std::forward<const T&>(keys)...); }

};

}
