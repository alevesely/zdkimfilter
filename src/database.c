/*
** database.c - written in milano by vesely on 25sep2012
** read/write via odbx
*/
/*
* zdkimfilter - Sign outgoing, verify incoming mail messages

Copyright (C) 2012 Alessandro Vesely

This file is part of zdkimfilter

zdkimfilter is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

zdkimfilter is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License version 3
along with zdkimfilter.  If not, see <http://www.gnu.org/licenses/>.

Additional permission under GNU GPLv3 section 7:

If you modify zdkimfilter, or any covered work, by linking or combining it
with software developed by The OpenDKIM Project and its contributors,
containing parts covered by the applicable licence, the licensor or
zdkimfilter grants you additional permission to convey the resulting work.
*/
#include <config.h>
#if !ZDKIMFILTER_DEBUG
#define NDEBUG
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdint.h>
#include <limits.h>
#include <syslog.h>
#include <time.h>
#include <sys/time.h>
#if defined HAVE_OPENDBX
#include <opendbx/api.h>
#endif // HAVE_OPENDBX
#include "database.h"
#include <assert.h>

static logfun_t do_report = &syslog;


#if defined HAVE_OPENDBX
/*
* Each query/statement has a number of allowed variables that may or may not
* be used.  They are passed as parameters to the relevant function
*/

#define DATABASE_VARIABLE(x) x##_variable,
typedef enum variable_id
{
	not_used_variable, // last stmt part, sentinel, ...
	#include "database_variables.h"
	DB_SQL_VAR_SIZE,
	FLAG_VAR_SIZE = (DB_SQL_VAR_SIZE + 7) & -8 // round up
} variable_id;
#undef DATABASE_VARIABLE

#define STRING2(P) #P
#define STRING(P) STRING2(P)
#define DATABASE_VARIABLE(x) STRING(x),
static const char * const variable_name[] =
{
	"", // not used variable
	#include "database_variables.h"
	NULL
};	

#undef DATABASE_VARIABLE

// high bit: allowed in statement
// rest: number of uses in a statement (max 127 times)
typedef struct flags_var
{
	unsigned char var[FLAG_VAR_SIZE];
} flags_var;

static inline int var_is_allowed(flags_var const *flags, variable_id id)
{ return (flags->var[id] & 0x80U) != 0; }
static inline int var_is_used(flags_var const *flags, variable_id id)
{ return flags->var[id] & 0x7fU; }
static inline void set_var_used(flags_var *flags, variable_id id)
{ flags->var[id] =
	flags->var[id] & 0x80U | 0x7fU & (var_is_used(flags, id) + 1); }
	
// make sure a 32-bit flag holds all the variables we use;
typedef int
compile_time_check_that_FLAG_VAR_SIZE_lt_32[FLAG_VAR_SIZE < 32? 1: -1];
typedef uint32_t var_flag_t;

static void set_var_allowed(flags_var *flags, var_flag_t bitflag)
{
	memset(flags, 0, DB_SQL_VAR_SIZE);
	variable_id id = 0;
	var_flag_t mask = 1;
	for (; bitflag; bitflag &= ~mask, mask <<= 1, ++id)
		if (bitflag & mask)
			flags->var[id] |= 0x80U;
}

typedef struct stmt_part
{
	char *snippet;
	size_t length;
	variable_id id;
} stmt_part;

typedef struct stmt_compose
{
	flags_var flags;
	uint32_t length;
	uint32_t count;
	stmt_part part[]; 
} stmt_compose;

static int
search_var(char *p, flags_var *flags, char **q, size_t *sz, stmt_part *part)
{
	char *s = strchr(p, '$');
	if (s && s[1] == '(')
	{
		char *name = &s[2], *e = name;
		int ch;
		while (isalnum(ch = *(unsigned char*)e) || ch == '_')
			++e;

		variable_id id = DB_SQL_VAR_SIZE;
		size_t length = e - name;

		if (length && ch == ')')
			for (id = 1; id < DB_SQL_VAR_SIZE; ++id)
				if (strncmp(variable_name[id], name, length) == 0)
					break;

		if (id >= DB_SQL_VAR_SIZE || !var_is_allowed(flags, id))
		{
			(*do_report)(LOG_ERR,
				"Malformed or invalid variable near: %.*s",
					(int)(e - p), p);
			return -1;
		}

		length += 3;  // for '$', '(', and ')': length of text to be removed
		set_var_used(flags, id);
		*sz += length;
		*q = e;
		if (part)
		{
			part->length = length; // to be changed by caller
			part->id = id;
		}
	}
	else
	{
		*q = NULL;
	}

	return 0;
}

static int count_vars(flags_var *flags, char *src, size_t *sz, stmt_part *part)
{
	int count = 0;
	char *p = src, *dest;

	if (part)
		dest = part->snippet;

	for (;;)
	{
		char *q;
		if (search_var(p, flags, &q, sz, part))
			return -1;

		if (part && *p)
		{
			part->snippet = dest;
			if (q)
				// turn length of tail into length of head
				part->length = q - p + 1 - part->length;
			else
				part->length = strlen(p);

			memcpy(dest, p, part->length);
			dest[part->length] = 0;

			dest += part->length + 1;
			part += 1;
		}

		if (q)
		{
			++count;
			p = q + 1;
		}
		else
			return count + (*p != 0); // one more for the last snippet
	}
}

static stmt_compose *stmt_alloc(flags_var *flags, char *src)
{
	size_t sz = 0;
	int count = count_vars(flags, src, &sz, NULL);

	if (count <= 0)
		return NULL;

	/*
	* allocate room for count snippets, which means
	* the last part may be without variable;
	*/
	size_t const length = strlen(src) - sz; // sum of snippets, excluding term 0
	size_t const array = count * sizeof(stmt_part);
	size_t const alloc = sizeof (stmt_compose) + array + length + count;
	stmt_compose *stmt = malloc(alloc);
	if (stmt == NULL)
	{
		(*do_report)(LOG_ALERT, "MEMORY FAULT");
		return NULL;
	}

	memset(&stmt->part[0], 0, array);
	stmt->flags = *flags;
	stmt->length = length;
	stmt->count = count;
	stmt->part[0].snippet = (char*)&stmt->part[count];
	count_vars(flags, src, &sz, &stmt->part[0]);

	assert(stmt->part[count-1].snippet + strlen(stmt->part[count-1].snippet) ==
		(char*)stmt + alloc - 1);

	return stmt;
}

//////////////////////////////////////////////////////////////
typedef enum stmt_id
{
	db_sql_whitelisted,
	db_sql_select_domain,
	db_sql_update_domain,
	db_sql_insert_domain,
	db_sql_insert_msg_ref,
	db_sql_insert_message,

	total_statements
} stmt_id;

// keep in sync by hands
static const char*stmt_name[] =
{
	"db_sql_whitelisted",
	"db_sql_select_domain",
	"db_sql_update_domain",
	"db_sql_insert_domain",
	"db_sql_insert_msg_ref",
	"db_sql_insert_message"
};

// typedef'd in .h
struct db_work_area
{
	odbx_t *handle;
	odbx_result_t *result;
	stmt_compose *stmt[total_statements];
	char *var[DB_SQL_VAR_SIZE];

	db_parm_t z;

	time_t pending_result;
	char pending_result_msg;

	char is_test;
};

db_parm_t* db_parm_addr(db_work_area *dwa) { return dwa? &dwa->z: NULL; }

void db_clear(db_work_area* dwa)
{
	if (dwa)
	{
		if (dwa->handle)
		{
			int err = odbx_unbind(dwa->handle);
			if (err)
				(*do_report)(LOG_ERR, "Error unbinding odbx handle: %s",
					odbx_error(dwa->handle, err));
			err = odbx_finish(dwa->handle);

			if (err)
				(*do_report)(LOG_ERR, "Error closing odbx handle: %s",
					odbx_error(dwa->handle, err));
		}
		for (int i = 0; i < DB_SQL_VAR_SIZE; ++i)
			free(dwa->var[i]);
		for (int i = 0; i < total_statements; ++i)
			free(dwa->stmt[i]);
		free(dwa);
	}
}

db_work_area *db_init(void)
/*
* this must be the first function called.  Parse SQL statements.
*/
{
	do_report = set_parm_logfun(NULL);  // use that logging function

	db_work_area *dwa = calloc(1, sizeof(db_work_area));
	if (dwa == NULL)
	{
		(*do_report)(LOG_ALERT, "MEMORY FAULT");
		return NULL;
	}

	// options not set will be left alone
	dwa->z.db_opt_compress = dwa->z.db_opt_multi_statements = -1;
	dwa->z.db_opt_paged_results = -1;
	return dwa;
}

int db_config_wrapup(db_work_area *dwa)
/*
* return the number of db_sql_* statements configured, -1 for fatal error;
*/
{
	int fatal = -1;
	int count = 0;
	if (dwa)
	{
		fatal = 0;

#define STMT_ALLOC(STMT, BITFLAG) \
		if (dwa->z.STMT) { ++count; \
			flags_var flags; \
			set_var_allowed(&flags, BITFLAG); \
			if ((dwa->stmt[STMT] = stmt_alloc(&flags, dwa->z.STMT)) == NULL) \
				fatal = -1; \
		} else (void)0

		static const var_flag_t message_variables =
			1 << ino_variable | 1 << mtime_variable | 1 << pid_variable |
			1 << ip_variable | 1 << date_variable | 1 << message_id_variable |
			1 << content_type_variable | 1 << content_encoding_variable |
			1 << received_count_variable | 1 << signatures_count_variable |
			1 << mailing_list_variable;

		STMT_ALLOC(db_sql_whitelisted, 1 << domain_variable);

		STMT_ALLOC(db_sql_select_domain,
			1 << domain_variable | 1 << auth_type_variable |
			message_variables);

		STMT_ALLOC(db_sql_update_domain,
			1 << domain_variable | 1 << auth_type_variable |
			1 << domain_ref_variable |
			message_variables);

		STMT_ALLOC(db_sql_insert_domain,
			1 << domain_variable | 1 << auth_type_variable |
			message_variables);

		STMT_ALLOC(db_sql_insert_msg_ref,
			1 << domain_variable | 1 << auth_type_variable |
			1 << domain_ref_variable | 1 << message_ref_variable |
			message_variables);
			
		STMT_ALLOC(db_sql_insert_message, message_variables);

#undef STMT_ALLOC

		if (dwa->z.db_timeout <= 0)
			dwa->z.db_timeout = 2;
	}
	return fatal? fatal: count;
}

static int clear_pending_result(db_work_area* dwa)
/*
* Some DB backends seem to be unable to fetch results promptly.  The doc says:
*
*   If a timeout or error occurs, the result pointer is set to NULL. In case
*   of a timeout, odbx_result() should be called again because the query
*   isn't canceled. This function must be called multiple times until it
*   returns zero, even if the query contains only one statement. Otherwise,
*   memory will be leaked and odbx_query() will return an error.
*/
{
	assert(dwa);

	if (dwa->pending_result)
	{
		odbx_result_t *result;
		struct timeval timeout;
		timeout.tv_sec = dwa->z.db_timeout;
		timeout.tv_usec = 0;

		int err = odbx_result(dwa->handle, &result, &timeout, 0 /* chunk */);
		if (result)
		{
			int err2 = odbx_result_finish(result);
			time_t now = time(NULL);
			(*do_report)(LOG_NOTICE,
				"DB server result discarded after %ld seconds (err=%d, %d)",
				(long)(now - dwa->pending_result), err, err2);
			dwa->pending_result = 0;
		}
		else if (dwa->pending_result_msg == 0)
		{
			time_t now = time(NULL);
			(*do_report)(LOG_CRIT,
				"DB server stuck after %ld seconds: %s (err=%d)",
					(long)(now - dwa->pending_result),
					odbx_error(dwa->handle, err), err);
			dwa->pending_result_msg = 1;  // limit logs to 1 per message
		}
	}

	return dwa->pending_result? -1: 0;
}

#define OTHER_ERROR (-100 - (ODBX_MAX_ERRNO))

static int dump_vars(db_work_area* dwa, stmt_id sid, var_flag_t bitflag)
{
	FILE *fp = fopen("database_dump", "a");
	if (fp)
	{
		assert(sid < total_statements);
		fprintf(fp, "Variables for statement %s:\n", stmt_name[sid]);

		variable_id id = 0;
		var_flag_t mask = 1, bit;
		for (bit = bitflag; bit; bit &= ~mask, mask <<= 1, ++id)
		{
			assert((bitflag & mask) == 0 || dwa->var[id] != NULL);

			if (bitflag & mask)
				fprintf(fp, "%s: %s\n", variable_name[id], dwa->var[id]);
		}
		fputc('\n', fp);
		fclose(fp);
	}
	return 0;
}

static int stmt_run(db_work_area* dwa, stmt_id sid, var_flag_t bitflag,
	char **wantchar, int* wantint)
/*
* Build a statement assembling snippets and arguments, then run it.
* count is the number of pairs of arguments that follow.
*
* Whitelits queries should just return a numeric result within [-1000, 1000]
*
* After inserting a message, or after querying or inserting domain, a reference
* variable can be returned.  Those queries must be conceived so as to return a
* single value that will become the message_ref or domain_ref variable.  This
* can be done by explicitely SELECT LAST_INSERT_ID() after the insertion, using
* multi-statement.  Otherwise those variables will be undefined, and replaced
* with an empty string when used.
*
* Return 1 if a result is found (and returned in any of wantchar and wantint),
* 0 if no result was found.  If an error is found (and logged)
* return OTHER_ERROR.
*/
{
	assert(dwa);
	assert(sid < total_statements);

	if (dwa->is_test)
		return dump_vars(dwa, sid, bitflag);

	odbx_t *const handle = dwa->handle;
	stmt_compose const *const stmt = dwa->stmt[sid];
	if (handle == NULL || stmt == NULL || clear_pending_result(dwa))
		return OTHER_ERROR;

	size_t arglen[DB_SQL_VAR_SIZE];
	memset(arglen, 0, sizeof arglen);

	size_t length = 0;
	variable_id id = 0;
	var_flag_t mask = 1, bit;
	for (bit = bitflag; bit; bit &= ~mask, mask <<= 1, ++id)
	{
		assert((bitflag & mask) == 0 || dwa->var[id] != NULL);

		// arglen[id] remains 0 for variables that are not actually given,
		// albeit allowed and used.  They are replaced with an empty string.
		if (bitflag & mask)
		{
			int const use = var_is_used(&stmt->flags, id);
			if (use)
				length += 2 * use * (arglen[id] = strlen(dwa->var[id]));
		}
	}

	size_t alloc = stmt->length + length + 1;
	char *sql = malloc(alloc), *p = sql, *ep = sql + alloc;

	if (sql == NULL)
	{
		(*do_report)(LOG_ALERT, "MEMORY FAULT");
		return OTHER_ERROR;
	}

	variable_id last_id = 0;
	uint32_t i;
	for (i = 0; i < stmt->count; ++i)
	{
		stmt_part const * const part = &stmt->part[i];

		if (part->length)
		{
			char *next = p + part->length;
			if (next >= ep)
			{
				(*do_report)(last_id? LOG_WARNING: LOG_CRIT,
					"Escape space exhausted at step %d, after $(%s)",
						i, last_id? variable_name[last_id]: "<internal error>");
				break;
			}

			memcpy(p, part->snippet, part->length);
			p = next;
		}

		variable_id const id = part->id;
		if (id && arglen[id]) // not_used_variable or empty strings don't play
		{
			size_t l = ep - p;
			int err = odbx_escape(handle, dwa->var[id], arglen[id], p, &l);
			char *next = p + l;
			if (err || next >= ep)
			{
				(*do_report)(LOG_WARNING,
					"Bad %s name %.63s (length=%zu) cannot be queried: %s",
						variable_name[id], dwa->var[id], arglen[id],
							err? odbx_error(handle, err): "escape space exhausted");
				break;
			}

			last_id = id;
			p = next;
		}
	}

	if (i < stmt->count) // error during the loop
	{
		free(sql);
		return OTHER_ERROR;
	}

	*p = 0;
	int err = odbx_query(handle, sql, p - sql);
	if (err != ODBX_ERR_SUCCESS)
	{
		(*do_report)(LOG_ERR, "DB error: %s (query: %s)",
			odbx_error(handle, err), sql);
		free(sql);
		return OTHER_ERROR;
	}
#if defined TEST_MAIN
	(*do_report)(LOG_DEBUG, "query: %s", sql);
#endif

	/*
	* All queries return at most a single value.  So we get the first
	* (numeric) column for it.
	*/

	int got_result = 0;
	for (int r_set = 1;; ++r_set)
	{
		odbx_result_t *result = NULL;
		struct timeval timeout;
		timeout.tv_sec = dwa->z.db_timeout;
		timeout.tv_usec = 0;

		int err = odbx_result(handle, &result, &timeout, 0 /* chunk */);
#if defined TEST_MAIN
		(*do_report)(LOG_DEBUG, "part #%d: rc=%d, result=%sNULL",
			r_set, err, result == NULL? "": "non-");
#endif

		if (err == ODBX_RES_DONE)
			break;

		if (err == ODBX_RES_NOROWS)
		{
			uint64_t rows = odbx_rows_affected(result);
			if (rows > 1)
				(*do_report)(LOG_WARNING,
					"part #%d of the query affected %ld rows (query: %s)",
						r_set, rows, sql);
			odbx_result_finish(result);
		}

		else if (err == ODBX_RES_ROWS)
		{
			unsigned long seen = 0;
			for (;;)
			{
				int fetch_more = odbx_row_fetch(result);
				if (fetch_more <= 0)
					break;

				++seen;
				if (got_result == 0 && odbx_column_count(result) > 0)
				{
					got_result = 1;
					char const *field = odbx_field_value(result, 0);
					if (field != NULL)
					{
						if (wantchar &&
							(*wantchar = strdup(field)) == NULL)
								(*do_report)(LOG_ALERT, "MEMORY FAULT");
						if (wantint)
						{
							char *t = NULL;
							long l = strtol(field, &t, 0);
							if (t && *t == 0 && l >= 0 && l < INT_MAX && l > INT_MIN)
								*wantint = (int)l;
							else
							{
								*wantint = *field != 0;
								(*do_report)(LOG_WARNING,
									"part #%d of the query returned a non-number %s"
									" converted to %d (query: %s)",
									r_set, field, *wantint, sql);
							}
						}
#if defined TEST_MAIN
						(*do_report)(LOG_DEBUG, "row#%ld: %s", seen, field);
#endif
					}
				}
			}
			if (seen > 1)
			{
				(*do_report)(LOG_WARNING,
					"part #%d of the query had %ld rows (query: %s)",
						r_set, seen, sql);
			}
			odbx_result_finish(result);
		}

		else if (err < 0)
		{
			(*do_report)(LOG_ERR, "DB error: %s (err: %d, part #%d, query: %s)",
				odbx_error(handle, err), err, r_set, sql);
			if (result)
				odbx_result_finish(result);
			break;
		}

		else if (err == ODBX_RES_TIMEOUT)
		{
			(*do_report)(LOG_ERR,
				"DB timeout: %d secs is too low? (part #%d, query: %s)",
				dwa->z.db_timeout, r_set, sql);
			dwa->pending_result = time(0);
			dwa->pending_result_msg = 0;
			err =  OTHER_ERROR;
			if (result)
				odbx_result_finish(result);
			break;
		}

		else
		{
			(*do_report)(LOG_CRIT,
				"Internal error: unexpected rc=%d in part #%d of query %s",
				err, r_set, sql);
			if (result)
				odbx_result_finish(result);
			break;
		}
	}

	free(sql);
	return got_result;
}

static inline int do_set_option(odbx_t *handle,
	int code, int value, char const *opt, char const *opt_name)
/*
* either value or opt are passed to odbx.
* assume thet integer values can neve be -1.
*/
{
	int err = odbx_set_option(handle, code,
		value == -1? (void*)opt: (void*)&value);
	if (err)
	{
		int errtype = odbx_error_type(handle, err);
		if (opt)
			(*do_report)(errtype? LOG_CRIT: LOG_WARNING,
				"%s error setting %s to \"%s\": %s",
				errtype? "Fatal": "Transient",
				opt_name, opt, odbx_error(handle, err));
		else
			(*do_report)(errtype? LOG_CRIT: LOG_WARNING,
				"%s error setting %s to %d: %s",
				errtype? "Fatal": "Transient",
				opt_name, value, odbx_error(handle, err));
		if (errtype)
		{
			odbx_finish(handle);
			return -1;
		}
	}
	return 0;
}

static int set_enable_disable_option(odbx_t *handle,
	int code, int opt, char const *opt_name)
{
	int value;
	switch (opt)
	{
		case 0:
			value = ODBX_DISABLE;
			break;
		case 1:
			value = ODBX_ENABLE;
			break;
		default:
			return 0;
	}
	return do_set_option(handle, code, value, NULL, opt_name);
}

int db_connect(db_work_area *dwa)
/*
* if inited (dwa != NULL) connect to server
* return 0, or -1 on unexpected error
*/
{
	if (dwa == NULL)
		return 0;

	odbx_t *handle;
	if (dwa->z.db_backend == NULL)
	{
		(*do_report)(LOG_CRIT, "Missing db_backend: cannot connect");
		return -1;
	}
	else if (strcmp(dwa->z.db_backend, "test") == 0)
	{
		dwa->is_test = 1;
		return 0;
	}

	int err = odbx_init(&handle, dwa->z.db_backend,
		dwa->z.db_host, dwa->z.db_port);
	if (err)
	{
		(*do_report)(LOG_CRIT,
			"Unable to initialize ODBX with backend=%s, host=%s, port=%s: %s",
			dwa->z.db_backend,
			dwa->z.db_host? dwa->z.db_host: "NULL",
			dwa->z.db_port? dwa->z.db_port: "NULL",
			odbx_error(NULL, err));
		return -1;
	}

	// option TLS (enum)
	if (dwa->z.db_opt_tls)
	{
		int value;
		switch (*dwa->z.db_opt_tls)
		{
			case 'A':
			case 'a':
				value = ODBX_TLS_ALWAYS;
				break;
			case 'N':
			case 'n':
				value = ODBX_TLS_NEVER;
				break;
			case 'T':
			case 't':
				value = ODBX_TLS_TRY;
				break;
			default:
				(*do_report)(LOG_WARNING,
					"Invalid value \"%s\" for db_opt_tls: "
					"use \"ALWAYS\", \"TRY\", or \"NEVER\"",
						dwa->z.db_opt_tls);
				value = -1;
				break;
		}
		if (value != -1 &&
			do_set_option(handle, ODBX_OPT_TLS, value,
			dwa->z.db_opt_tls, "db_opt_tls") == -1)
				return -1;
	}

	// option MULTI_STATEMENTS
	if (dwa->z.db_opt_multi_statements &&
		set_enable_disable_option(handle, ODBX_OPT_MULTI_STATEMENTS,
			dwa->z.db_opt_multi_statements, "db_opt_multi_statements") == -1)
				return -1;

	// option PAGED_RESULTS (int != -1)
	if (dwa->z.db_opt_paged_results != -1 &&
		do_set_option(handle, ODBX_OPT_PAGED_RESULTS,
		dwa->z.db_opt_paged_results, NULL, "db_opt_paged_results") == -1)
			return -1;

	// option COMPRESS
	if (dwa->z.db_opt_compress &&
		set_enable_disable_option(handle, ODBX_OPT_COMPRESS,
			dwa->z.db_opt_compress, "db_opt_compress") == -1)
				return -1;

	// option MODE (string)
	if (dwa->z.db_opt_mode &&
		do_set_option(handle, ODBX_OPT_MODE,
			-1, dwa->z.db_opt_mode, "db_opt_mode") == -1)
				return -1;

	// bind
	err = odbx_bind(handle, dwa->z.db_database,
		dwa->z.db_user, dwa->z.db_password, ODBX_BIND_SIMPLE);
	if (err)
	{
		(*do_report)(LOG_CRIT,
			"Cannot bind to %s (user: %s, %s): %s",
			dwa->z.db_database? dwa->z.db_database: "NULL",
			dwa->z.db_user? dwa->z.db_user: "NULL",
			dwa->z.db_password?
				*dwa->z.db_password? "using password: yes":
					"using empty password": "using password: no",
			odbx_error(handle, err));
		odbx_finish(handle);
		return -1;
	}

	dwa->handle = handle;
	return 0;
}

static int test_whitelisted(db_work_area* dwa, char const* domain)
{
	char *const h = dwa->z.db_sql_whitelisted;
	if (h)
	{
		char *x = strstr(h, domain);
		if (x)
		{
			x += strlen(domain);
			if (*x == ':')
				++x;
			return atoi(x);
		}
	}
	return 0;
}

int db_is_whitelisted(db_work_area* dwa, char const* domain)
/*
* Run db_sql_whitelisted query for domain.  If the query is a statement,
* return 0 if it affected no rows, 1 otherwise (log a warning if > 1 rows).
* If the query is a SELECT, and the result contains a row with a non-null
* value in the first column, if that field is a positive int return that
* value, otherwise return 1 if there was such a row, otherwise 0 (log a
* warning if more than one row or column are found).
*
* The numeric result should be >= 1 if the domain is found, where > 1
* implies some trust.  0 or negative values may cause signatures to be
* ignored.
*/
{
	assert(dwa == NULL || dwa->handle || dwa->is_test);
	assert(domain);

	if (dwa == NULL)
		return 0;

#if !defined NDEBUG
	for (int i = 0; i < DB_SQL_VAR_SIZE; ++i)
		assert(dwa->var[i] == NULL || i == ip_variable);
#endif
	if (dwa->is_test)
		return test_whitelisted(dwa, domain);

	int rtc = 0;
	dwa->var[domain_variable] = (char*)domain;  // cast away const
	stmt_run(dwa, db_sql_whitelisted, 1 << domain_variable, NULL, &rtc);
	dwa->var[domain_variable] = NULL;

	return rtc;
}

extern char *ip_to_hex(char const *ip); // ip_to_hex.c
void db_set_client_ip(db_work_area *dwa, char const *ip)
{
	assert(dwa);
	assert(ip);

	dwa->var[ip_variable] = ip_to_hex(ip);
}


#if 0
static inline int makeint2(int const ch, char const **p)
{
	assert(isdigit(ch));
	assert(**(unsigned char const **)p == ch);

	int ch2, r = ch - '0';
	if (isdigit(ch2 = *(unsigned char const*)++*p) && ch2 != 0)
	{
		r *= 10;
		r += ch2 - '0';
		*p += 1;
	}
	return r;
}

static char*date_convert(char const *date)
{
	if (date == NULL) return NULL;

	struct tm tm;
	memset(&tm, 0, sizeof tm);

	char const *p = date;
	int ch;
	while (!isdigit(ch = *(unsigned char const*)p) && ch != 0)
		++p;
	if (ch == 0) return NULL;

	tm.tm_mday = makeint2(ch, &p);
	while (isspace(ch = *(unsigned char const*)p) && ch != 0)
		++p;
	if (!isalpha(ch)) return NULL;

	static const char *month[12] = {"Jan", "Feb", "Mar", "Apr",
		"May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	int i;
	for (i = 0; i < 12; ++i)
		if (strncasecmp(p, month[i], 3) == 0)
			break;
	if (i >= 12) return NULL;

	tm.tm_mon = i;
	p += 3;
	while (!isdigit(ch = *(unsigned char const*)p) && ch != 0)
		++p;
	if (ch == 0) return NULL;

	char *t = NULL;
	long l = strtoul(p, &t, 10);
	if (l < 1900 || l > INT_MAX) return NULL;

	tm.tm_year = l - 1900;
	p = t;
	while (!isdigit(ch = *(unsigned char const*)p) && ch != 0)
		++p;
	if (ch == 0) return NULL;

	tm.tm_hour = makeint2(ch, &p);
	if	(*p != ':') return NULL;
	if (!isdigit(ch = *(unsigned char const*)++p)) return NULL;

	tm.tm_min = makeint2(ch, &p);
	if	(*p != ':') return NULL;
	if (!isdigit(ch = *(unsigned char const*)++p)) return NULL;

	tm.tm_sec = makeint2(ch, &p);
	while (isspace(ch = *(unsigned char const*)p) && ch != 0)
		++p;

	if (ch)
	{
		time_t t;
		int tz = 0;
		char *save_tz = getenv("TZ");
		char const *new_tz;
		tm.tm_isdst = -1;

		if (ch && strchr("+-", ch))
		{
			int sign = ch == '+'? 1: -1;
			if (!isdigit(ch = *(unsigned char const*)++p)) return NULL;

			tz = 60 * makeint2(ch, &p);
			if (!isdigit(ch = *(unsigned char const*)p)) return NULL;

			tz += makeint2(ch, &p);
			tz *= 60*sign;
			new_tz = ""; // GMT
		}
		else
			new_tz = p; // doesn't work

		setenv("TZ", p, 1);
		tzset();
		t = mktime(&tm);
		if (save_tz)
			setenv("TZ", save_tz, 1);
		else
			unsetenv("TZ");
		tzset();
		if (t == (time_t)-1) return NULL;

		t -= tz;
		if (localtime_r(&t, &tm) == NULL) return NULL;
	}

	char buf[80];
	sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d",
		tm.tm_year + 1900, tm.tm_mday, tm.tm_mon + 1,
		tm.tm_hour, tm.tm_min, tm.tm_sec);
	return strdup(buf);
}
#endif

static void comma_copy(char *buf, char const *value, int *comma)
{
	if (*comma)
		strcat(buf, ",");
	strcat(buf, value);
	*comma = 1;
}

void db_set_stats_info(db_work_area* dwa, stats_info const*info)
{
	assert(dwa);
	assert(info);
#if !defined NDEBUG
	for (int i = 0; i < DB_SQL_VAR_SIZE; ++i)
		assert(dwa->var[i] == NULL || i == ip_variable);
#endif

	if (info == NULL || info->domain_head == NULL)
	{
		(*do_report)(LOG_CRIT,
			"Internal error, invalid stats for id: %s",
			info && info->ino_mtime_pid? info->ino_mtime_pid: "unknown");
		return;
	}

	var_flag_t bitflag = 0, zeroflag = 0;
	if (info->ino_mtime_pid != NULL)
	{
		char const *mtime = strchr(info->ino_mtime_pid, '.');
		if (mtime && mtime[1])
		{
			char const *pid = strchr(mtime + 1, '.');
			if (pid && pid[1])
			{
				if ((dwa->var[pid_variable] = strdup(&pid[1])) != NULL)
					bitflag |= 1 << pid_variable;
				size_t l = pid - mtime;
				char *p = malloc(l);
				if ((dwa->var[mtime_variable] = p) != NULL)
				{
					memcpy(p, &mtime[1], l);
					p[l-1] = 0;
					bitflag |= 1 << mtime_variable;
				}
				l = mtime - info->ino_mtime_pid + 1;
				p = malloc(l);
				if ((dwa->var[ino_variable] = p) != NULL)
				{
					memcpy(p, info->ino_mtime_pid, l);
					p[l-1] = 0;
					bitflag |= 1 << ino_variable;
				}
			}
		}
	}

	if (!dwa->is_test &&
		bitflag != (1 << ino_variable | 1 << mtime_variable | 1 << pid_variable))
	{
		(*do_report)(LOG_CRIT,
			"Internal error at " __FILE__ ":%d: missing %s", __LINE__,
			info->ino_mtime_pid == NULL? "ino_mtime_pid":
			(bitflag & 1 << pid_variable) == 0? "pid_variable":
			(bitflag & 1 << mtime_variable) == 0? "mtime_variable":
			(bitflag & 1 << ino_variable) == 0? "ino_variable": "something");
		return;
	}

	if (dwa->var[ip_variable] != NULL) // this may have been set in its own call
		bitflag |= 1 << ip_variable;

#define SET_STRING(N) \
	if ((dwa->var[N##_variable] = strdup(info->N)) != NULL) \
		bitflag |= 1 << N##_variable; else (void)0
	SET_STRING(date);
	SET_STRING(message_id);
	SET_STRING(content_type);
	SET_STRING(content_encoding);
#undef SET_STRING

	// these must be zeroed from dwa->var before they are freed:
	// that's what zeroflag is for.
	// we assume no number takes more than 10 chars to print.
	char buf[40], *p = buf, *safe_stop = &buf[sizeof buf - 10];
#define SET_NUMBER(N) \
	if (p < safe_stop) { \
		p += 1 + sprintf(dwa->var[N##_variable] = p, "%u", info->N); \
		var_flag_t bit = 1 << N##_variable; \
		bitflag |= bit; zeroflag |= bit; } else (void)0
	SET_NUMBER(received_count);
	SET_NUMBER(signatures_count);
	SET_NUMBER(mailing_list);
#undef SET_NUMBER

	/*
	* With the message variables in place, we can insert the message.
	*/
	char *var = NULL;
	int rc = stmt_run(dwa, db_sql_insert_message, bitflag, &var, NULL);

	if ((dwa->var[message_ref_variable] = var) != NULL)
		bitflag |= 1 << message_ref_variable;

	/*
	* Now for each domain, we try to query it, otherwise we insert it.
	* With the retrieved reference, we insert a msg_ref for each domain
	*/
	if (rc >= 0 && info->domain_head != NULL)
	{
		// author,spf,spf_helo,dkim,vbr
		// 1234567890123456789012345678
		char authbuf[32];
		dwa->var[auth_type_variable] = authbuf;
		var_flag_t bit2 = 1 << auth_type_variable | 1 << domain_variable;
		bitflag |= bit2;
		zeroflag |= bit2;

		bit2 = 1 << domain_ref_variable; // const during the loop
		for (domain_prescreen *dps = info->domain_head;
			dps != NULL; dps = dps->next)
		{
			int comma = 0;
			authbuf[0] = 0;
			if (dps->u.f.is_from)
				comma_copy(authbuf, "author", &comma);
			if (dps->u.f.is_helo)
				comma_copy(authbuf, "spf_helo", &comma);
			if (dps->u.f.is_mfrom)
				comma_copy(authbuf, "spf", &comma);
			if (dps->u.f.sig_is_ok)
				comma_copy(authbuf, "dkim", &comma);
			if (dps->u.f.vbr_is_ok)
			{
				comma_copy(authbuf, "vbr", &comma);
				dps->vbr_mv = "the_trusted_voucher.example";
			}

			dwa->var[domain_variable] = dps->name;
			bitflag &= ~bit2;

			int selected = 1;
			char *domain_ref = NULL;
			rc = stmt_run(dwa, db_sql_select_domain, bitflag, &domain_ref, NULL);
			if (rc < 0)
				continue;

			if (domain_ref == NULL)
			{
				selected = 0;
				rc = stmt_run(dwa, db_sql_insert_domain, bitflag, &domain_ref, NULL);
				if (rc < 0)
					continue;

				if (domain_ref == NULL)
					stmt_run(dwa, db_sql_select_domain, bitflag, &domain_ref, NULL);
			}

			if (domain_ref)
			{
				free(dwa->var[domain_ref_variable]);
				dwa->var[domain_ref_variable] = domain_ref;
				bitflag |= bit2;
			}

			if (selected)
				stmt_run(dwa, db_sql_update_domain, bitflag, NULL, NULL);

			stmt_run(dwa, db_sql_insert_msg_ref, bitflag, NULL, NULL);
		}
	}

	variable_id id = 0;
	var_flag_t mask = 1;
	for (; zeroflag; zeroflag &= ~mask, mask <<= 1, ++id)
		if (zeroflag & mask)
			dwa->var[id] = NULL;
}

#if defined TEST_MAIN

int main(int argc, char*argv[])
{
	size_t maxarglen = strlen(argv[0]);
	int rtc = 0, config = 0,
		query_whitelisted = argc,
		set_stats = argc,
		set_stats_domain = argc;
	char const *config_file = NULL;

	for (int i = 1; i < argc; ++i)
	{
		char const *const arg = argv[i];
		size_t arglen = strlen(arg);

		if (arglen > maxarglen)
			maxarglen = arglen;

		if (arg[0] != '-')
			continue;

		if (strcmp(arg, "-f") == 0)
		{
			config_file = ++i < argc ? argv[i] : NULL;
		}
		else if (strcmp(arg, "--config") == 0)
		{
			config = 1;
		}
		else if (strcmp(arg, "--version") == 0)
		{
			fputs(PACKAGE_NAME ", version " PACKAGE_VERSION "\n"
				"Compiled with"
#if defined NDEBUG
				"out"
#endif
				" debugging support\n", stdout);
			return 0;
		}
		else if (strcmp(arg, "--help") == 0)
		{
			printf("TESTdb command line args:\n"
				"  -f config-filename      override %s\n"
				"  --config                report configuration\n"
				"  --help                  print this stuff and exit\n"
				"  --version               print version string and exit\n"
				"  --db-sql-whitelisted    query remaining args\n"
				"  --set-stats             insert new message\n"
				"  --set-stats-domain      domains who sent the message\n"
				"For set-stats, the following arguments are expected:\n"
				" ino.mtime.pid, ip, date, message_id, content_type,\n"
				" content_encoding, received_count, signatures_count,\n"
				" and mailing_list.\n"
				"The domains must be given one per argument, using commas to\n"
				"separate the tokens, which are the domain name, followed by\n"
				"any of: author, spf, dkim, vbr\n",
					default_config_file);
			return 0;
		}
		else if (strcmp(arg, "--db-sql-whitelisted") == 0)
		{
			query_whitelisted = i + 1;
		}
		else if (strcmp(arg, "--set-stats") == 0)
		{
			set_stats = i + 1;
		}
		else if (strcmp(arg, "--set-stats-domain") == 0)
		{
			set_stats_domain = i + 1;
		}
	}
	set_parm_logfun(&stderrlog);
	db_work_area *dwa = db_init();
	if (dwa == NULL)
		return 1;

	if (config_file == NULL)
		config_file = default_config_file;

	void *parm_target[PARM_TARGET_SIZE];
	parm_target[parm_t_id] = NULL;
	parm_target[db_parm_t_id] = db_parm_addr(dwa);

	if (*config_file &&
		read_all_values(parm_target, config_file))
	{
		db_clear(dwa);
		return 1;
	}

	db_config_wrapup(dwa);
	
	if (config)
		print_parm(parm_target);

	if (db_connect(dwa) == 0)
	{
		stats_info stats;
		memset(&stats, 0, sizeof stats);
		stats.domain_head = calloc(argc, sizeof(domain_prescreen) + maxarglen + 1);
		for (int i = query_whitelisted; i < argc; ++i)
		{
			if (argv[i][0] == '-')
				break;
			printf("%s: %d\n", argv[i], db_is_whitelisted(dwa, argv[i]));
		}
		if (stats.domain_head)
		{
			int m = 0;
			for (int i = set_stats_domain; i < argc; ++i)
			{
				char *arg = argv[i];
				if (arg[0] == '-')
					break;

				strcpy(stats.domain_head[m].name, arg);
				char *comma = strchr(stats.domain_head[m].name, ',');
				if (comma)
					*comma = 0;

				while ((arg = strchr(arg, ',')) != NULL)
				{
					*arg++ = 0;
					if (strncmp(arg, "author", 6) == 0)
						stats.domain_head[m].u.f.is_from = 1;
					else if (strncmp(arg, "spf", 3) == 0)
						stats.domain_head[m].u.f.is_mfrom = 1;
					else if (strncmp(arg, "spf_helo", 8) == 0)
						stats.domain_head[m].u.f.is_helo = 1;
					else if (strncmp(arg, "dkim", 4) == 0)
						stats.domain_head[m].u.f.sig_is_ok = 1;
					else if (strncmp(arg, "vbr", 3) == 0)
						stats.domain_head[m].u.f.vbr_is_ok = 1;
					else if (*arg)
						printf("invalid domain token \"%s\"\n", arg);
				}
				stats.domain_head[m].next = &stats.domain_head[m+1];
				++m;
			}

			if (m > 0)
				stats.domain_head[m-1].next = NULL;

			int n = 0;
			for (int i = set_stats; i < argc; ++i)
			{
				char *arg = argv[i];
				if (arg[0] == '-')
					break;
				if (arg[0] == 0)
					continue;

				switch(n)
				{
					case 0: stats.ino_mtime_pid = arg; break;
					case 1: db_set_client_ip(dwa, arg); break;
					case 2: stats.date = arg; break;
					case 3: stats.message_id = arg; break;
					case 4: stats.content_type = arg; break;
					case 5: stats.content_encoding = arg; break;
					case 6: stats.received_count = atoi(arg); break;
					case 7: stats.signatures_count = atoi(arg); break;
					case 8: stats.mailing_list = atoi(arg); break;
					default:
						printf("extra set-stats argument \"%s\" ignored\n",
							arg);
						break;
				}
				++n;
			}
			if (n && m && stats.domain_head)
				db_set_stats_info(dwa, &stats);
			free(stats.domain_head);
		}
	}

	clear_parm(parm_target);
	db_clear(dwa);
	return rtc;
}
#endif // TEST_MAIN
#endif // HAVE_OPENDBX
