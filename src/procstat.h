#ifndef _PROCSTAT_H_
#define _PROCSTAT_H_

#include <stdint.h>
#include <linux/types.h>
#include <stddef.h>
#include <unistd.h>
#include <stdio.h>

struct procstat_context;
struct procstat_item;

/**
 * @brief: stats formatter method
 * @param object registered with statistics
 * @param buffer to format object to
 * @param lenght of buffer
 * @param offset to return from beginning of buffer. This can be ignored in case statistics string
 * is large > 4K
 */
typedef ssize_t (*procstats_formatter)(void *object, char *buffer, size_t length, off_t offset);

/**
 * @brief registration parameter for simple value statistics
 * @name of statistics
 * @bject to be passed to the formatter.
 * @fmt data formatter
 */
struct procstat_simple_handle {
	const char 	    *name;
	void 	 	    *object;
	procstats_formatter fmt;
};

/**
 * @brief create statstics context and mount it on running machine under @mountpoint.
 * @mountpoint root directory for statistics will be created in case it does not exists.
 * @return context to be used for all statistics operations. or NULL in case of error. errno will be
 * set accordingly
 */
struct procstat_context *procstat_create(const char *mountpoint);

/**
 * @brief unregister and destroy all registered statistics
 */
void procstat_destroy(struct procstat_context *context);

/**
 * @return get root item under which statstics can be registered
 */
struct procstat_item *procstat_root(struct procstat_context *context);

/**
 * @brief blocking method to handle statstics operations. This must be run from
 * dedicated thread exactly once.
 */
void procstat_loop(struct procstat_context *context);

/**
 * @brief create directory @name under @parent directory
 * @context statistics context
 * @parent directory under which to create directory with @name, in case parent is NULL
 * 	    @name will be created under root directory
 * @name of the directory to create.
 * @return created directory or NULL in case of failure and errno will be set accordingly
 */
struct procstat_item *procstat_create_directory(struct procstat_context *context,
					  	struct procstat_item *parent,
						const char *name);

/**
 * @brief creates counter, which will be exposed as @name under @parent dictory.
 * @return 0 on success, -1  in case of failure and errno will be set accordingly
 */
int procstat_create_simple(struct procstat_context *context,
			   struct procstat_item *parent,
			   struct procstat_simple_handle *descriptors,
			   size_t descriptors_len);

#define DEFINE_PROCSTAT_FORMATTER(__type, __fmt, __fmt_name)\
static inline ssize_t procstat_format_ ## __type ##_## __fmt_name(void *object, char *buffer, size_t len, off_t offset)\
{\
	return snprintf(buffer, len, __fmt, *((__type *)object));\
}\

#ifndef u64
#define u64 uint64_t
#endif

#ifndef u32
#define u32 uint32_t
#endif


DEFINE_PROCSTAT_FORMATTER(u64, "%lu\n", decimal);
DEFINE_PROCSTAT_FORMATTER(u64, "%lx\n", hex);
DEFINE_PROCSTAT_FORMATTER(u64, "0x%lx\n", address);
DEFINE_PROCSTAT_FORMATTER(u32, "%u\n", decimal);
DEFINE_PROCSTAT_FORMATTER(u32, "%x\n", hex);

#define DEFINE_PROCSTAT_SIMPLE_ATTRIBUTE(__type)\
static inline int procstat_create_ ## __type(struct procstat_context *context, struct procstat_item *parent, const char *name, __type *object)\
{\
	struct procstat_simple_handle descriptor = {name, object, procstat_format_ ## __type ## _decimal};\
	\
	return procstat_create_simple(context, parent, &descriptor, 1);\
}\

DEFINE_PROCSTAT_SIMPLE_ATTRIBUTE(u32);
DEFINE_PROCSTAT_SIMPLE_ATTRIBUTE(u64);

/**
 * @brief defines formatter with getter method. This can be used to provide standard POD formatting with custom
 * get function to fetch object value
 */
#define DEFINE_PROCSTAT_CUSTOM_FORMATTER(name, getter_function, __type, __fmt)\
static inline ssize_t procstat_format_ ## __type ##_## name(void *object, char *buffer, size_t length, off_t offset)\
{\
	__type out;\
	\
	getter_function((__type *)object, &out);\
	return snprintf(buffer, length, __fmt, out);\
}\

/**
 * @brief registration parameter for start end statistics. This is equivalent to creating a directory
 * with @name and registering start and end files bounded to @start and @end
 * @name of statistics
 * @start object to be exposed as "start" filename under @name statistic
 * @end object to be exposed as "end" filename under @name statistic
 * @fmt data formatter
 */
struct procstat_start_end_handle {
	char *name;
	void *start;
	void *end;
	procstats_formatter fmt;
};


struct procstat_start_end_u32 {
	uint32_t start;
	uint32_t end;
};

struct procstat_start_end_u64 {
	uint64_t start;
	uint64_t end;
};

/**
 * @brief create start end statistics
 * @context of the stats
 * @parent under which to create start end statistics
 * @descriptors array of descriptors for start end statistics
 * @descriptors_len of the array
 */
int procstat_create_start_end(struct procstat_context 	       *context,
			      struct procstat_item 	       *parent,
			      struct procstat_start_end_handle *descriptors,
			      size_t 				descriptors_len);

/**
 * @brief shortcut to create handle for u32 start end stat
 */
#define procstat_start_end_u32_handle(name, start_end)\
	(struct procstat_start_end_handle){name, &start_end.start, &start_end.end, procstat_format_u32_decimal}

/**
 * @brief shortcut to create handle for u64 start end stat
 */
#define procstat_start_end_u64_handle(name, start_end)\
	(struct procstat_start_end_handle){name, &start_end.start, &start_end.end, procstat_format_u64_decimal}

/**
 * @brief registration parameter for series statistics. statistical analysis will be performed on values
 * submitted as series point. mean and variance will be calculated upon even point submittion
 * stddev and average are additionally exposed and calculated upen request.
 * @name of statistics
 * @object to be passed to the formatter.
 * @fmt data formatter
 */
struct procstat_series_u64_handle {
	const char *name;
	struct procstat_series_u64 *series;
};


struct procstat_series_u64 {
	uint64_t sum;
	uint64_t count;
	uint64_t min;
	uint64_t max;
	uint64_t last;
	uint64_t mean;
	uint64_t aggregated_variance;
};

/**
 * @brief create series statistics.
 */
int procstat_create_u64_series(struct procstat_context *context, struct procstat_item *parent,
			       const char *name, struct procstat_series_u64 *series);


/**
 * @brief create multiple series statstics
 */
int procstat_create_multiple_u64_series(struct procstat_context *context,
					struct procstat_item *parent,
					struct procstat_series_u64_handle *descriptors,
					size_t series_len);

/**
 * @brief add points to series statistics
 */
void procstat_u64_series_add_point(struct procstat_series_u64 *series, uint64_t value);

/**
 * @brief removes statistics item previosly created with any of creation methods
 */
void procstat_remove(struct procstat_context *context, struct procstat_item *item);

/**
 * @brief searches for @name item under @parent directory and removes it
 * @return 0 in case of success or error code
 */
int procstat_remove_by_name(struct procstat_context *context, struct procstat_item *parent, const char *name);


#endif