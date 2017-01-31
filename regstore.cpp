#include "regstore.hpp"

namespace mark {

const char *regstore::errstr(regstore::err error)
{
	switch (error) {
	case err::ok: return "OK";
	case err::invalid_key: return "Invalid key";
	case err::invalid_value: return "Invalid value";
	case err::unknown: return "Unknown error";
	case err::not_readable: return "Not readable";
	case err::not_writeable: return "Not writeable";
	default: return "Unknown error code";
	}
}

regstore::register_list regstore::_list(const std::string& remote = nullptr) const
{
	register_list res;
	for (const auto& kv : store) {
		const auto& name = kv.first;
		register_info info;
		reg_type& rt = info.type;
		rt = rt_none;
		if (kv.second.first != nullptr) {
			rt |= rt_readable;
		}
		if (kv.second.second != nullptr) {
			rt |= rt_writeable;
		}
		info.subscribed = !remote.empty() && _query_observer(name, remote, info.sub_info);
		res.emplace(name, info);
	}
	return res;
}

void regstore::_add(const std::string& key, const regstore::getter& get, const regstore::setter& set)
{
	if (!store.emplace(key, std::make_pair(get, set)).second) {
		throw std::logic_error("Attempted to add key \"" + key + "\" to register store twice");
	}
}

regstore::err regstore::_set(const std::string& key, const std::string& value)
{
	/* Could be const, but intentionally not */
	const auto& it = store.find(key);
	if (it == store.end()) {
		return err::invalid_key;
	}
	const auto& func = it->second.second;
	if (func == nullptr) {
		return err::not_writeable;
	}
	err res;
	try {
		res = func(value);
	} catch (const std::invalid_argument&) {
		res = err::invalid_value;
	}
	if (res == err::ok) {
		_send_notification(key, value);
	}
	return res;
}

regstore::err regstore::_get(const std::string& key, std::string& value) const
{
	const auto& it = store.find(key);
	if (it == store.end()) {
		return err::invalid_key;
	}
	const auto& func = it->second.first;
	if (func == nullptr) {
		return err::not_readable;
	}
	err res;
	try {
		res = func(value);
	} catch (const std::invalid_argument&) {
		res = err::invalid_value;
	}
	return res;
}

void regstore::_observe(const std::string& key, const std::string& remote, const regstore::observer& obs, const std::chrono::steady_clock::duration& min_interval)
{
	if (obs == nullptr) {
		_unobserve(key, remote);
		return;
	}
	auto& ob = observers[key][remote];
	ob.func = obs;
	ob.next = {};
	ob.min_interval = min_interval;
}

void regstore::_unobserve(const std::string& key, const std::string& remote)
{
	const auto for_reg = observers.find(key);
	if (for_reg == observers.end()) {
		return;
	}
	const auto for_rem = for_reg->second.find(remote);
	if (for_rem == for_reg->second.end()) {
		return;
	}
	for_reg->second.erase(for_rem);
	if (for_reg->second.empty()) {
		observers.erase(for_reg);
	}
}

bool regstore::_query_observer(const std::string& key, const std::string& remote, subscription_info& info) const
{
	const auto for_reg = observers.find(key);
	if (for_reg == observers.end()) {
		return false;
	}
	const auto for_rem = for_reg->second.find(remote);
	if (for_rem == for_reg->second.end()) {
		return false;
	}
	const auto& rec = for_rem->second;
	/* Slice */
	info = rec;
	return true;
}

void regstore::_send_notification(const std::string& key, const std::string& value) const
{
	const auto for_reg = observers.find(key);
	if (for_reg == observers.end()) {
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	for (auto rem : for_reg->second) {
		observer_entry& ob = rem.second;
		if (now < ob.next) {
			continue;
		}
		ob.next = now;
		ob.next += ob.min_interval;
		ob.func(value);
	}
}

regstore::err regstore::_notify(const std::string& key) const
{
	std::string value;
	auto res = _get(key, value);
	if (res == err::ok) {
		_send_notification(key, value);
	}
	return res;
}

}
