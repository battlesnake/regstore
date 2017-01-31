#pragma once
#include <cstd/std.h>
#include <fixedstr/fixedstr.h>
#include <cstruct/binary_tree.h>

enum regstore_err {
	regstore_err_ok,
	regstore_err_invalid_key,
	regstore_err_invalid_value,
	regstore_err_unknown,
	regstore_err_not_readable,
	regstore_err_not_writeable,
	regstore_err_no_change
};

const char *regstore_errstr(enum regstore_err error);

typedef enum regstore_err regstore_getter(void *arg, struct fstr *value);
typedef enum regstore_err regstore_setter(void *arg, const struct fstr *value);
typedef void regstore_observer(void *arg, const struct fstr *value);

/* Register type flags (used by register info */
enum regstore_regtype {
	rt_none = 0,
	rt_readable = 1,
	rt_writeable = 2
};

/* Subscription info (query response) */
struct regstore_subscription_info {
	int64_t next_ms; /* TODO: replace int64_t with time type once tempus lib is ready */
	int64_t min_interval_ms;
};

/* Register info (query response) */
struct regstore_reginfo {
	struct fstr name;
	enum regstore_regtype type;
	struct fstr value;
	struct regstore_subscription_info sub_info;
	/* Not used in main tree, only for info listing */
	bool subscribed;
};

/* Register store */
struct regstore {
	struct binary_tree store; /* reg(name) */
};

void regstore_init(struct regstore *inst);
void regstore_destroy(struct regstore *inst);

/* List registers (pass uninitialised/zero-filled binary_tree in, tree<regstore_reginfo> returned) */
bool regstore_list(struct regstore *inst, struct binary_tree *out, const struct fstr *remote, bool values);

/* Add a register */
bool regstore_add(struct regstore *inst, const struct fstr *key, regstore_getter *getter, void *getter_arg, regstore_setter *setter, void *setter_arg);
bool regstore_add_s(struct regstore *inst, const char *key, regstore_getter *getter, void *getter_arg, regstore_setter *setter, void *setter_arg);

/* Delete a register */
bool regstore_delete(struct regstore *inst, const struct fstr *key);

/* Set a register */
enum regstore_err regstore_set(struct regstore *inst, const struct fstr *key, const struct fstr *value);

/* Get a register */
enum regstore_err regstore_get(struct regstore *inst, const struct fstr *key, struct fstr *value);

/* Subscribe / unsubscribe */
bool regstore_observe(struct regstore *inst, const struct fstr *key, const struct fstr *remote, regstore_observer *observer, void *observer_arg, int64_t min_interval);

bool regstore_unobserve(struct regstore *inst, const struct fstr *key, const struct fstr *remote);

/* Get observer info */
bool regstore_query_observer(struct regstore *inst, const struct fstr *key, const struct fstr *remote, struct regstore_subscription_info *out);

/* Notify that register has changed */
enum regstore_err regstore_notify(struct regstore *inst, const struct fstr *key);
