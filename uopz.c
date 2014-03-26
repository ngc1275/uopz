/*
  +----------------------------------------------------------------------+
  | uopz                                                                 |
  +----------------------------------------------------------------------+
  | Copyright (c) Joe Watkins 2012 - 2014                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Joe Watkins <krakjoe@php.net>                                |
  +----------------------------------------------------------------------+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "Zend/zend_closures.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_extensions.h"
#include "Zend/zend_string.h"
#include "Zend/zend_vm_opcodes.h"

#include "uopz.h"
#include "compile.h"
#include "copy.h"

ZEND_DECLARE_MODULE_GLOBALS(uopz)

#define MAX_OPCODE 163
#undef EX
#define EX(element) execute_data->element
#define OPLINE EX(opline)
#define OPCODE OPLINE->opcode

#if PHP_VERSION_ID >= 50500
# define EX_T(offset) (*EX_TMP_VAR(execute_data, offset))
#else
# define EX_T(offset) (*(temp_variable *)((char*)execute_data->Ts + offset))
#endif

dtor_func_t uopz_original_class_dtor = NULL;

static zend_bool uopz_backup(zend_class_entry *scope, zval *name TSRMLS_DC);

dtor_func_t php_uopz_original_class_dtor = NULL;

PHP_INI_BEGIN()
	 STD_PHP_INI_ENTRY("uopz.backup",     "1",    PHP_INI_SYSTEM,    OnUpdateBool,       ini.backup,             zend_uopz_globals,        uopz_globals)
PHP_INI_END()

/* {{{ */
user_opcode_handler_t ohandlers[MAX_OPCODE]; /* }}} */

/* {{{ */
typedef struct _uopz_opcode_t {
	zend_uchar   code;
	const char  *name;
	size_t       length;
} uopz_opcode_t;

#define UOPZ_CODE(n)   {n, #n, sizeof(#n)-1}
#define UOPZ_CODE_END  {ZEND_NOP, NULL, 0L}

uopz_opcode_t uoverrides[] = {
	UOPZ_CODE(ZEND_NEW),
	UOPZ_CODE(ZEND_THROW),
	UOPZ_CODE(ZEND_ADD_TRAIT),
	UOPZ_CODE(ZEND_ADD_INTERFACE),
	UOPZ_CODE(ZEND_INSTANCEOF),
	UOPZ_CODE_END
}; /* }}} */

/* {{{ */
typedef struct _uopz_magic_t {
	const char *name;
	size_t      length;
	int         id;
} uopz_magic_t;

#define UOPZ_MAGIC(name, id) {name, sizeof(name)-1, id}
#define UOPZ_MAGIC_END	     {NULL, 0, 0L}

uopz_magic_t umagic[] = {
	UOPZ_MAGIC(ZEND_CONSTRUCTOR_FUNC_NAME, 0),
	UOPZ_MAGIC(ZEND_DESTRUCTOR_FUNC_NAME, 1),
	UOPZ_MAGIC(ZEND_CLONE_FUNC_NAME, 2),
	UOPZ_MAGIC(ZEND_GET_FUNC_NAME, 3),
	UOPZ_MAGIC(ZEND_SET_FUNC_NAME, 4),
	UOPZ_MAGIC(ZEND_UNSET_FUNC_NAME, 5),
	UOPZ_MAGIC(ZEND_ISSET_FUNC_NAME, 6),
	UOPZ_MAGIC(ZEND_CALL_FUNC_NAME, 7),
	UOPZ_MAGIC(ZEND_CALLSTATIC_FUNC_NAME, 8),
	UOPZ_MAGIC(ZEND_TOSTRING_FUNC_NAME, 9),
	UOPZ_MAGIC("serialize", 10),
	UOPZ_MAGIC("unserialize", 11),
#ifdef ZEND_DEBUGINFO_FUNC_NAME
	UOPZ_MAGIC(ZEND_DEBUGINFO_FUNC_NAME, 12),
#endif
	UOPZ_MAGIC_END
};
/* }}} */

/* {{{ */
typedef struct _uopz_handler_t {
	zval *handler;
} uopz_handler_t; /* }}} */

/* {{{ */
typedef struct _uopz_backup_t {
	zend_class_entry *scope;
	char             *name;
	size_t            length;
	zend_ulong        hash;
	zend_function     internal;
} uopz_backup_t; /* }}} */

/* {{{ */
static void php_uopz_init_globals(zend_uopz_globals *ng) {
	ng->overload._exit = NULL;
	ng->ini.backup = 1;
} /* }}} */

/* {{{ */
static void php_uopz_handler_dtor(void *pData) {
	uopz_handler_t *uhandler = (uopz_handler_t*) pData;

	if (uhandler->handler) {
		zval_ptr_dtor(&uhandler->handler);
	}
} /* }}} */

/* {{{ */
static void php_uopz_backup_dtor(void *pData) {
	uopz_backup_t *backup = (uopz_backup_t *) pData;
	zend_function *restored = NULL;
	HashTable *table = NULL;
	TSRMLS_FETCH();
	
	table = backup->scope ?
		&backup->scope->function_table :
		CG(function_table);
	
	if (backup->scope)
		backup->scope->refcount--;
	
	if (backup->internal.type == ZEND_USER_FUNCTION) {
		destroy_zend_function(&backup->internal TSRMLS_CC);
	} else {
		if ((zend_hash_quick_find(table, 
				backup->name, backup->length, 
				backup->hash, (void**)&restored) == SUCCESS) && 
			(restored->type == ZEND_USER_FUNCTION)) {
			zend_hash_quick_update(
				table, backup->name, backup->length,
				backup->hash, (void**) &backup->internal,
				sizeof(zend_function), (void**) &restored);
		}
	}
	
	efree(backup->name);
} /* }}} */

/* {{{ */
static void php_uopz_backup_table_dtor(void *pData) {
	zend_hash_destroy((HashTable*) pData);
} /* }}} */

/* {{{ */
static int php_uopz_handler(ZEND_OPCODE_HANDLER_ARGS) {
	uopz_handler_t *uhandler = NULL;
	int dispatching = 0;

	if (zend_hash_index_find(&UOPZ(overload).table, OPCODE, (void**)&uhandler) == SUCCESS) {
		zend_fcall_info fci;
		zend_fcall_info_cache fcc;
		char *cerror = NULL;
		zval *retval = NULL;
		zval *op1 = NULL,
			 *op2 = NULL,
			 *result = NULL;
		zend_free_op free_op1, free_op2, free_result;
		zend_class_entry *oce = NULL, **nce = NULL;

		memset(
			&fci, 0, sizeof(zend_fcall_info));

		if (zend_is_callable_ex(uhandler->handler, NULL, IS_CALLABLE_CHECK_SILENT, NULL, NULL, &fcc, &cerror TSRMLS_CC)) {
			if (zend_fcall_info_init(uhandler->handler, 
					IS_CALLABLE_CHECK_SILENT, 
					&fci, &fcc, 
					NULL, &cerror TSRMLS_CC) == SUCCESS) {

				fci.params = (zval***) emalloc(2 * sizeof(zval **));
				fci.param_count = 2;

				switch (OPCODE) {
					case ZEND_INSTANCEOF: {
						GET_OP1(BP_VAR_R);

						oce = EX_T(OPLINE->op2.var).class_entry; 

						MAKE_STD_ZVAL(op2);
						ZVAL_STRINGL(op2, oce->name, oce->name_length, 1);
						fci.params[1] = &op2;
					} break;
				
					case ZEND_ADD_INTERFACE:
					case ZEND_ADD_TRAIT: {
						oce = EX_T(OPLINE->op1.var).class_entry;

						MAKE_STD_ZVAL(op1);
						ZVAL_STRINGL(op1, oce->name, oce->name_length, 1);
						fci.params[0] = &op1;
					
						if (CACHED_PTR(OPLINE->op2.literal->cache_slot)) {
							oce = CACHED_PTR(OPLINE->op2.literal->cache_slot);
						} else {
							oce = zend_fetch_class_by_name(
								Z_STRVAL_P(OPLINE->op2.zv), Z_STRLEN_P(OPLINE->op2.zv), 
								OPLINE->op2.literal + 1, ZEND_FETCH_CLASS_NO_AUTOLOAD TSRMLS_CC);
						}

						MAKE_STD_ZVAL(op2);
						ZVAL_STRINGL(op2, oce->name, oce->name_length, 1);
						fci.params[1] = &op2;
						fci.param_count = 2;
					} break;

					case ZEND_NEW: {
						oce = EX_T(OPLINE->op1.var).class_entry;

						MAKE_STD_ZVAL(op1);
						ZVAL_STRINGL(op1, oce->name, oce->name_length, 1);
						fci.param_count = 1;
						fci.params[0] = &op1;
					} break;

					default: {
						GET_OP1(BP_VAR_RW);
						GET_OP2(BP_VAR_RW);
					}
				}
			
				fci.retval_ptr_ptr = &retval;

				zend_try {
					zend_call_function(&fci, &fcc TSRMLS_CC);
				} zend_end_try();

				if (fci.params)
					efree(fci.params);

				if (retval) {
					if (Z_TYPE_P(retval) != IS_NULL) {
						convert_to_long(retval);
						dispatching = 
							Z_LVAL_P(retval);
					}
					zval_ptr_dtor(&retval);
				}

				switch (OPCODE) {
					case ZEND_INSTANCEOF: {
						convert_to_string(op2);

						if (zend_lookup_class(Z_STRVAL_P(op2), Z_STRLEN_P(op2), &nce TSRMLS_CC) == SUCCESS) {
							if (*nce != oce) {
								EX_T(OPLINE->op2.var).class_entry = *nce;
							}
						}

						zval_ptr_dtor(&op2);
					} break;

					case ZEND_ADD_INTERFACE:
					case ZEND_ADD_TRAIT: {
						convert_to_string(op2);

						if (zend_lookup_class(Z_STRVAL_P(op2), Z_STRLEN_P(op2), &nce TSRMLS_CC) == SUCCESS) {
							if (*nce != oce) {
								CACHE_PTR(OPLINE->op2.literal->cache_slot, *nce);
							}
						}

						zval_ptr_dtor(&op1);
						zval_ptr_dtor(&op2);
					} break;

					case ZEND_NEW: {
						convert_to_string(op1);

						if (zend_lookup_class(Z_STRVAL_P(op1), Z_STRLEN_P(op1), &nce TSRMLS_CC) == SUCCESS) {
							if (*nce != oce) {
								EX_T(OPLINE->op1.var).class_entry = *nce;
							}
						}

						zval_ptr_dtor(&op1);
					} break;
				}
			}
		}
		
	}

	if (dispatching) switch(dispatching) {
		case ZEND_USER_OPCODE_CONTINUE:
			EX(opline)++;

		default:
			return dispatching;
	}

	if (ohandlers[OPCODE]) {
		return ohandlers[OPCODE]
			(ZEND_OPCODE_HANDLER_ARGS_PASSTHRU);
	}

	return ZEND_USER_OPCODE_DISPATCH_TO | OPCODE;
} /* }}} */

/* {{{ */
static inline void php_uopz_backup(TSRMLS_D) {
	HashTable *table = CG(function_table);
	HashPosition position[2];
	zval name;
	
	for (zend_hash_internal_pointer_reset_ex(table, &position[0]);
		 zend_hash_get_current_key_ex(table, &Z_STRVAL(name), &Z_STRLEN(name), NULL, 0, &position[0]) == HASH_KEY_IS_STRING;
		 zend_hash_move_forward_ex(table, &position[0])) {
		 if (--Z_STRLEN(name))
			uopz_backup(NULL, &name TSRMLS_CC);	 
	}
	
	table = CG(class_table);
	
	{
		zend_class_entry **clazz = NULL;
		
		for (zend_hash_internal_pointer_reset_ex(table, &position[0]);
			 zend_hash_get_current_data_ex(table, (void**) &clazz, &position[0]) == SUCCESS;
			 zend_hash_move_forward_ex(table, &position[0])) {
			 if (!(*clazz)->ce_flags & ZEND_ACC_INTERFACE) {
			 	for (zend_hash_internal_pointer_reset_ex(&(*clazz)->function_table, &position[1]);
			 		 zend_hash_get_current_key_ex(&(*clazz)->function_table, &Z_STRVAL(name), &Z_STRLEN(name), NULL, 0, &position[1]) == HASH_KEY_IS_STRING;
			 		 zend_hash_move_forward_ex(&(*clazz)->function_table, &position[1])) {
			 		 if (--Z_STRLEN(name))
			 		 	uopz_backup((*clazz), &name TSRMLS_CC);
			 	}
			 }
		}	
	}
} /* }}} */

/* {{{ */
static inline void php_uopz_class_dtor(void *pData) {
	zend_class_entry **clazz = (zend_class_entry**) pData;
	
	if (--(*clazz)->refcount)
		return;
	
	if (php_uopz_original_class_dtor) {
		php_uopz_original_class_dtor(pData);
	}
} /* }}} */

/* {{{ PHP_MINIT_FUNCTION
 */
static PHP_MINIT_FUNCTION(uopz)
{
	ZEND_INIT_MODULE_GLOBALS(uopz, php_uopz_init_globals, NULL);

	UOPZ(copts) = CG(compiler_options);

	CG(compiler_options) |= 
		(ZEND_COMPILE_HANDLE_OP_ARRAY);
		
	php_uopz_original_class_dtor = CG(class_table)->pDestructor;
	CG(class_table)->pDestructor = php_uopz_class_dtor;

	memset(ohandlers, 0, sizeof(user_opcode_handler_t) * MAX_OPCODE);

	{
#define REGISTER_ZEND_OPCODE(name, len, op) \
	zend_register_long_constant\
		(name, len, op, CONST_CS|CONST_PERSISTENT, module_number TSRMLS_CC)

		uopz_opcode_t *uop = uoverrides;

		REGISTER_ZEND_OPCODE("ZEND_EXIT", sizeof("ZEND_EXIT"), ZEND_EXIT);

		while (uop->code != ZEND_NOP) {
			zval constant;

			ohandlers[uop->code] = 
				zend_get_user_opcode_handler(uop->code);
			zend_set_user_opcode_handler(uop->code, php_uopz_handler);
			if (!zend_get_constant(uop->name, uop->length+1, &constant TSRMLS_CC)) {
				REGISTER_ZEND_OPCODE(uop->name, uop->length+1, uop->code);
			} else zval_dtor(&constant);
			
			uop++;
		}

#undef REGISTER_ZEND_OPCODE
	}

	REGISTER_LONG_CONSTANT("ZEND_USER_OPCODE_CONTINUE", ZEND_USER_OPCODE_CONTINUE, CONST_CS|CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("ZEND_USER_OPCODE_ENTER", ZEND_USER_OPCODE_ENTER, CONST_CS|CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("ZEND_USER_OPCODE_LEAVE", ZEND_USER_OPCODE_LEAVE, CONST_CS|CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("ZEND_USER_OPCODE_DISPATCH", ZEND_USER_OPCODE_DISPATCH, CONST_CS|CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("ZEND_USER_OPCODE_DISPATCH_TO", ZEND_USER_OPCODE_DISPATCH_TO, CONST_CS|CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("ZEND_USER_OPCODE_RETURN", ZEND_USER_OPCODE_RETURN, CONST_CS|CONST_PERSISTENT);

	REGISTER_INI_ENTRIES();

	return SUCCESS;
}
/* }}} */

/* {{{ */
static PHP_MSHUTDOWN_FUNCTION(uopz)
{
	CG(compiler_options) = UOPZ(copts);
	
	UNREGISTER_INI_ENTRIES();

	CG(class_table)->pDestructor = php_uopz_original_class_dtor;

	return SUCCESS;
} /* }}} */

/* {{{ PHP_RINIT_FUNCTION
 */
static PHP_RINIT_FUNCTION(uopz)
{
	zend_hash_init(
		&UOPZ(overload).table, 8, NULL,
		(dtor_func_t) php_uopz_handler_dtor, 0);	
	zend_hash_init(
		&UOPZ(backup), 8, NULL, 
		(dtor_func_t) php_uopz_backup_table_dtor, 0);
	
	if (UOPZ(ini).backup) {
		php_uopz_backup(TSRMLS_C);
	}
	
	return SUCCESS;
} /* }}} */

/* {{{ PHP_RSHUTDOWN_FUNCTION
 */
static PHP_RSHUTDOWN_FUNCTION(uopz)
{
	if (UOPZ(overload)._exit) {
		zval_ptr_dtor(&UOPZ(overload)._exit);
	}

	zend_hash_destroy(&UOPZ(overload).table);
	zend_hash_destroy(&UOPZ(backup));
	
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
static PHP_MINFO_FUNCTION(uopz)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "uopz support", "enabled");
	php_info_print_table_end();
}
/* }}} */

/* {{{ */
static inline void php_uopz_overload_exit(zend_op_array *op_array) {

	zend_uint it = 0;
	zend_uint end = op_array->last;
	zend_op  *opline = NULL;
	TSRMLS_FETCH();

	while (it < end) {
		opline = &op_array->opcodes[it];

		switch (opline->opcode) {
			case ZEND_EXIT: {
				znode     result;
				zval      call;

				ZVAL_STRINGL(&call,
					"__uopz_exit_overload", 
					sizeof("__uopz_exit_overload")-1, 1);

				opline->opcode = ZEND_DO_FCALL;
				SET_NODE(op_array, opline->op1, call);
				SET_UNUSED(opline->op2);
#if PHP_VERSION_ID > 50500
				opline->op2.num = CG(context).nested_calls;
#endif
				opline->result.var = php_uopz_temp_var(op_array TSRMLS_CC);
				opline->result_type = IS_TMP_VAR;
				GET_NODE(op_array, &result, opline->result);		
				CALCULATE_LITERAL_HASH(op_array, opline->op1.constant);
				GET_CACHE_SLOT(op_array, opline->op1.constant);
			} break;
		}

		it++;
	}
} /* }}} */

/* {{{ proto void uopz_overload(int opcode, Callable handler) */
static PHP_FUNCTION(uopz_overload)
{
	zval *handler = NULL;
	long  opcode = ZEND_NOP;
	zend_fcall_info_cache fcc;
	char *cerror = NULL;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l|z", &opcode, &handler) != SUCCESS) {
		return;
	}

	if (!handler || Z_TYPE_P(handler) == IS_NULL) {
		if (opcode == ZEND_EXIT) {
			if (UOPZ(overload)._exit) {
				zval_ptr_dtor(&UOPZ(overload)._exit);
			
				UOPZ(overload)._exit = NULL;
			}
		} else {
			zend_hash_index_del(&UOPZ(overload).table, opcode);
		}
		
		RETURN_TRUE;
	}

	if (zend_is_callable_ex(handler, NULL, IS_CALLABLE_CHECK_SILENT, NULL, NULL, &fcc, &cerror TSRMLS_CC)) {
		if (opcode == ZEND_EXIT) {
			if (UOPZ(overload)._exit) {
				zval_ptr_dtor(&UOPZ(overload)._exit);
			}

			MAKE_STD_ZVAL(UOPZ(overload)._exit);
			ZVAL_ZVAL(UOPZ(overload)._exit, handler, 1, 0);
		} else {
			uopz_handler_t uhandler;

			MAKE_STD_ZVAL(uhandler.handler);
			ZVAL_ZVAL(uhandler.handler, handler, 1, 0);
			zend_hash_index_update(
				&UOPZ(overload).table, opcode, (void**) &uhandler, sizeof(uopz_handler_t), NULL);
		}

		RETURN_TRUE;
	}
}
/* }}} */

/* {{{ */
static zend_bool uopz_backup(zend_class_entry *scope, zval *name TSRMLS_DC) {
	HashTable     *backup = NULL;
	zend_function *function = NULL;
	size_t         lcl = Z_STRLEN_P(name)+1;
	char          *lcn = zend_str_tolower_dup(Z_STRVAL_P(name), lcl);
	zend_ulong     hash = zend_inline_hash_func(lcn, lcl);
	HashTable     *table = (scope) ? &scope->function_table : CG(function_table);
	
	if (zend_hash_quick_find(table, lcn, lcl, hash, (void**) &function) != SUCCESS) {
		efree(lcn);
		return;
	}
	
	if (zend_hash_index_find(&UOPZ(backup), (zend_ulong) table, (void**) &backup) != SUCCESS) {
		HashTable creating;
		
		zend_hash_init(&creating, 8, NULL, (dtor_func_t) php_uopz_backup_dtor, 0);
		zend_hash_index_update(	
			&UOPZ(backup),
			(zend_ulong) table,
			(void**) &creating,
			sizeof(HashTable), (void**) &backup);
	}
	
	if (!zend_hash_quick_exists(backup, lcn, lcl, hash)) {
		uopz_backup_t ubackup;
		
		ubackup.scope = scope;
		ubackup.name = lcn;
		ubackup.length = lcl;
		ubackup.hash = hash;
		ubackup.internal = uopz_copy_function(function TSRMLS_CC);
		
		if (zend_hash_quick_update(
			backup,
			lcn, lcl, hash,
			(void**) &ubackup,
			sizeof(uopz_backup_t), NULL) == SUCCESS) {
			if (scope)
				scope->refcount++;
		}
		
		return 1;
	} else {
		efree(lcn);
	}
	
	return 0;
} /* }}} */

/* {{{ proto bool uopz_backup(string class, string function)
       proto bool uopz_backup(string function) */
PHP_FUNCTION(uopz_backup) {
	zval *function = NULL;
	zval *overload = NULL;
	zend_class_entry *clazz = NULL;
	HashTable *table = CG(function_table);
	
	switch (ZEND_NUM_ARGS()) {
		case 2: if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Cz", &clazz, &function) != SUCCESS) {
			return;
		} else {
			table = &clazz->function_table;
		} break;
		
		case 1: if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &function) != SUCCESS) {
			return;
		} break;
	}
	
	RETURN_BOOL(uopz_backup(clazz, function TSRMLS_CC));
} /* }}} */

/* {{{ */
static inline zend_bool uopz_restore(HashTable *table, zval *name TSRMLS_DC) {
	zend_function *function = NULL;
	HashTable     *backup = NULL;
	size_t         lcl = Z_STRLEN_P(name)+1;
	char          *lcn = zend_str_tolower_dup(Z_STRVAL_P(name), lcl);
	zend_ulong     hash = zend_inline_hash_func(lcn, lcl);
	uopz_backup_t *ubackup = NULL;
	zend_function restored;
	
	if (zend_hash_index_find(&UOPZ(backup), (zend_ulong) table, (void**) &backup) != SUCCESS) {
		efree(lcn);
		return 0;
	}
	
	if (zend_hash_quick_find(backup, lcn, lcl, hash, (void**)&ubackup) != SUCCESS) {
		efree(lcn);
		return 0;
	}
	
	backup = ubackup->scope ?
		&ubackup->scope->function_table :
		CG(function_table);
		
	restored = uopz_copy_function(&ubackup->internal TSRMLS_CC);
	
	if (zend_hash_quick_update(
		backup, 
		ubackup->name, ubackup->length, ubackup->hash,
		(void**)&restored, sizeof(zend_function), NULL) != SUCCESS) {
		destroy_zend_function(&restored TSRMLS_CC);
	}
	
	efree(lcn);
	
	return 1;
} /* }}} */

/* {{{ proto bool uopz_restore(string class, string function)
	   proto bool uopz_restore(string function) */
PHP_FUNCTION(uopz_restore) {
	zval *function = NULL;
	zval *overload = NULL;
	zend_class_entry *clazz = NULL;
	HashTable *table = CG(function_table);
	
	switch (ZEND_NUM_ARGS()) {
		case 2: if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Cz", &clazz, &function) != SUCCESS) {
			return;
		} else {
			table = &clazz->function_table;
		} break;
		
		case 1: if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &function) != SUCCESS) {
			return;
		} break;
	}
	
	RETURN_BOOL(uopz_restore(table, function TSRMLS_CC));
} /* }}} */

/* {{{ */
static inline zend_bool uopz_rename(HashTable *table, zval *function, zval *overload TSRMLS_DC) {
	zend_function *tuple[2] = {NULL, NULL};
	zend_ulong hashed[2] = {0L, 0L};
	zend_function locals[2];
	zend_bool result = 1;
	
	if ((function && (Z_TYPE_P(function) == IS_STRING)) &&
		(overload && (Z_TYPE_P(overload) == IS_STRING))) {
		
		char *lcf = zend_str_tolower_dup(Z_STRVAL_P(function), Z_STRLEN_P(function)+1),
			 *lco = zend_str_tolower_dup(Z_STRVAL_P(overload), Z_STRLEN_P(overload)+1);
		
		hashed[0] = zend_inline_hash_func(lcf, Z_STRLEN_P(function)+1);
		hashed[1] = zend_inline_hash_func(lco, Z_STRLEN_P(overload)+1);
		
		zend_hash_quick_find(table, lcf, Z_STRLEN_P(function)+1, hashed[0], (void**) &tuple[0]);
		zend_hash_quick_find(table, lco, Z_STRLEN_P(overload)+1, hashed[1], (void**) &tuple[1]);
		
		if (tuple[0] && tuple[1]) {
			locals[0] = *tuple[0];
			locals[1] = *tuple[1];
			
			if (tuple[0]->type == ZEND_INTERNAL_FUNCTION && 
				tuple[1]->type == ZEND_INTERNAL_FUNCTION) {
				/* both internal */
			} else {
				if (tuple[0]->type == ZEND_INTERNAL_FUNCTION ||
					tuple[1]->type == ZEND_INTERNAL_FUNCTION) {
					/* one internal */
					if (tuple[0]->type == ZEND_INTERNAL_FUNCTION) {
						function_add_ref(&locals[1]);
					} else if (tuple[1]->type == ZEND_INTERNAL_FUNCTION) {
						function_add_ref(&locals[0]);
					}
				} else {
					/* both user */
					function_add_ref(&locals[0]);
					function_add_ref(&locals[1]);
				}
			}
			
			zend_hash_quick_update(table, 
				lcf, Z_STRLEN_P(function)+1, hashed[0], 
				(void**) &locals[1], sizeof(zend_function), NULL);
			zend_hash_quick_update(table,
				lco, Z_STRLEN_P(overload)+1, hashed[1],
				(void**) &locals[0], sizeof(zend_function), NULL);
		} else if (tuple[0] || tuple[1]) {
			/* only one existing function */
			locals[0] = tuple[0] ? *tuple[0] : *tuple[1];
			
			function_add_ref(&locals[0]);
			
			zend_hash_quick_update(table,
				lco, Z_STRLEN_P(overload)+1, hashed[1], 
				(void**) &locals[0], sizeof(zend_function), NULL);
		} else result = 0;
		
		efree(lcf);
		efree(lco);
		
	} else result = 0;
	
	return result;
} /* }}} */

/* {{{ proto bool uopz_rename(mixed function, mixed overload)
	   proto bool uopz_rename(string class, mixed function, mixed overload) */
PHP_FUNCTION(uopz_rename) {
	zval *function = NULL;
	zval *overload = NULL;
	zend_class_entry *clazz = NULL;
	HashTable *table = CG(function_table);
	
	switch (ZEND_NUM_ARGS()) {
		case 3: if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Czz", &clazz, &function, &overload) != SUCCESS) {
			return;
		} else {
			table = &clazz->function_table;
		} break;
		
		case 2: if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "zz", &function, &overload) != SUCCESS) {
			return;
		} break;
	}
	
	RETURN_BOOL(uopz_rename(table, function, overload TSRMLS_CC));
} /* }}} */

/* {{{ */
static inline zend_bool uopz_delete(HashTable *table, zval *function TSRMLS_DC) {
	zend_bool result = 0;
	
	if (function && (Z_TYPE_P(function) == IS_STRING)) {
		char *lcf = zend_str_tolower_dup
			(Z_STRVAL_P(function), Z_STRLEN_P(function)+1);
		zend_ulong hash = zend_inline_hash_func(lcf, Z_STRLEN_P(function)+1);
		
		if (zend_hash_quick_exists(table, lcf, Z_STRLEN_P(function)+1, hash)) {
			zend_hash_quick_del(
				table,
				lcf, Z_STRLEN_P(function)+1, hash);
			result = 1;
		}
		
		efree(lcf);
	}
	
	return result;
} /* }}} */

/* {{{ proto bool uopz_delete(mixed function)
	   proto bool uopz_delete(string class, mixed function) */
PHP_FUNCTION(uopz_delete) {
	zval *function = NULL;
	zend_class_entry *clazz = NULL;
	HashTable *table = CG(function_table);
	
	switch (ZEND_NUM_ARGS()) {
		case 2: if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Cz", &clazz, &function) != SUCCESS) {
			return;
		} else {
			table = &clazz->function_table;
		} break;
		
		case 1: if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &function) != SUCCESS) {
			return;
		} break;
	}
	
	RETURN_BOOL(uopz_delete(table, function TSRMLS_CC));
} /* }}} */

/* {{{ proto void __uopz_exit_overload() */
PHP_FUNCTION(__uopz_exit_overload) {
	zend_bool  leave = 1;

	if(UOPZ(overload)._exit) {
		zend_fcall_info fci;
		zend_fcall_info_cache fcc;
		char *cerror = NULL;
		zval *retval = NULL;

		memset(
			&fci, 0, sizeof(zend_fcall_info));
		if (zend_is_callable_ex(UOPZ(overload)._exit, NULL, IS_CALLABLE_CHECK_SILENT, NULL, NULL, &fcc, &cerror TSRMLS_CC)) {
			if (zend_fcall_info_init(UOPZ(overload)._exit, 
					IS_CALLABLE_CHECK_SILENT, 
					&fci, &fcc, 
					NULL, &cerror TSRMLS_CC) == SUCCESS) {

				fci.retval_ptr_ptr = &retval;

				zend_try {
					zend_call_function(&fci, &fcc TSRMLS_CC);
				} zend_end_try();

				if (retval) {
#if PHP_VERSION_ID > 50600
					leave = zend_is_true(retval TSRMLS_CC);
#else
					leave = zend_is_true(retval);
#endif
					zval_ptr_dtor(&retval);
				}
			}
		}
	}

	zval_ptr_dtor(&return_value);

	if (leave) {
		zend_bailout();
	}
} /* }}} */

/* {{{ */
static inline zend_bool uopz_redefine(HashTable *table, zval *constant, zval *variable TSRMLS_DC) {
	zend_constant *zconstant;
	zend_bool result = 0;
	zend_ulong     hash = zend_inline_hash_func(Z_STRVAL_P(constant), Z_STRLEN_P(constant)+1);
	
	switch (Z_TYPE_P(variable)) {
		case IS_LONG:
		case IS_DOUBLE:
		case IS_STRING:
		case IS_BOOL:
		case IS_RESOURCE:
		case IS_NULL:
			break;
			
		default:
			return result;			
	}
	
	if (Z_TYPE_P(constant) == IS_STRING) {
		if (zend_hash_quick_find(
			table, Z_STRVAL_P(constant), Z_STRLEN_P(constant)+1, 
			hash, (void**)&zconstant) == SUCCESS) {
			if ((table == EG(zend_constants))) {
				if (zconstant->module_number == PHP_USER_CONSTANT) {
					zval_dtor(&zconstant->value);
					ZVAL_ZVAL(
						&zconstant->value, variable, 1, 0);
					result = 1;
				}
			} else {
				zval *copy;
			
				MAKE_STD_ZVAL(copy);
				ZVAL_ZVAL(copy, variable, 1, 0);
				if (zend_hash_quick_update(
					table, 
					Z_STRVAL_P(constant), Z_STRLEN_P(constant)+1, 
					hash, (void**)&copy,
					sizeof(zval*), NULL) != SUCCESS) {
					zval_ptr_dtor(&copy);
				} else result = 1;
			}
		} else {
			if (table == EG(zend_constants)) {
				zend_constant create;
				
				ZVAL_ZVAL(&create.value, variable, 1, 0);
				create.flags = CONST_CS;
				create.name = zend_strndup(Z_STRVAL_P(constant), Z_STRLEN_P(constant));
				create.name_len = Z_STRLEN_P(constant)+1;
				create.module_number = PHP_USER_CONSTANT;
				
				if (zend_register_constant(&create TSRMLS_CC) != SUCCESS) {
					zval_dtor(&create.value);
				} else result = 1;
			} else {
				zval *create;
				
				MAKE_STD_ZVAL(create);
				ZVAL_ZVAL(create, variable, 1, 0);
				if (zend_hash_quick_update(
					table, 
					Z_STRVAL_P(constant), Z_STRLEN_P(constant)+1, 
					hash, (void**)&create,
					sizeof(zval*), NULL) != SUCCESS) {
					zval_ptr_dtor(&create);	
				} else result = 1;
			}
		}
	}
	
	return result;
} /* }}} */

/* {{{ proto bool uopz_redefine(string constant, mixed variable)
	   proto bool uopz_redefine(string class, string constant, mixed variable) */
PHP_FUNCTION(uopz_redefine)
{
	zval *constant = NULL;
	zval *variable = NULL;
	HashTable *table = EG(zend_constants);
	zend_class_entry *clazz = NULL;
	
	switch (ZEND_NUM_ARGS()) {
		case 3: {
			if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Czz", &clazz, &constant, &variable) != SUCCESS) {
				return;
			} else {
				table = &clazz->constants_table;
			}
		} break;
		
		default: {
			if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "zz", &constant, &variable) != SUCCESS) {
				return;
			}
		}
	}
	
	if (uopz_redefine(table, constant, variable TSRMLS_CC)) {
		if (table != EG(zend_constants)) {
			while ((clazz = clazz->parent)) {
				uopz_redefine(
					&clazz->constants_table, constant, variable TSRMLS_CC);
			}
		}	
		RETURN_TRUE;
	} else {
		RETURN_FALSE;
	}
} /* }}} */

/* {{{ */
static inline zend_bool uopz_undefine(HashTable *table, zval *constant TSRMLS_DC) {
	zend_bool result = 0;
	zend_constant *zconstant;
	zend_ulong     hash = zend_inline_hash_func(Z_STRVAL_P(constant), Z_STRLEN_P(constant)+1);
	
	if (Z_TYPE_P(constant) == IS_STRING && zend_hash_quick_find(
			table,
			Z_STRVAL_P(constant), Z_STRLEN_P(constant)+1, hash, (void**)&zconstant) == SUCCESS) {
		if (table == EG(zend_constants)) {
			if (zconstant->module_number == PHP_USER_CONSTANT) {
				if (zend_hash_quick_del
					(table, Z_STRVAL_P(constant), Z_STRLEN_P(constant)+1, hash) == SUCCESS) {
					result = 1;	
				}
			}
		} else {
			if (zend_hash_quick_del(table, Z_STRVAL_P(constant), Z_STRLEN_P(constant)+1, hash) == SUCCESS) {
				result = 1;
			}
		}
	}
	
	return result;
} /* }}} */

/* {{{ proto bool uopz_undefine(string constant) 
	   proto bool uopz_undefine(string class, string constant) */
PHP_FUNCTION(uopz_undefine)
{
	zval *constant = NULL;
	HashTable *table = EG(zend_constants);
	zend_class_entry *clazz = NULL;

	switch (ZEND_NUM_ARGS()) {
		case 2: {
			if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Cz", &clazz, &constant) != SUCCESS) {
				return;
			} else {
				table = &clazz->constants_table;
			}
		} break;
		
		default: {
			if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &constant) != SUCCESS) {
				return;
			}
		}
	}

	if (uopz_undefine(table, constant TSRMLS_CC)) {
		if (table != EG(zend_constants)) {
			while ((clazz = clazz->parent)) {
				uopz_undefine(&clazz->constants_table, constant TSRMLS_CC);
			}
		}
		RETURN_TRUE;
	} else {
		RETURN_FALSE;
	}
} /* }}} */

/* {{{ */
static inline zend_bool uopz_function(HashTable *table, zval *function, zend_function *override, zend_function **overridden TSRMLS_DC) {
	zend_bool result = 0;

	if (Z_TYPE_P(function) == IS_STRING) {
		char *lcname = zend_str_tolower_dup
			(Z_STRVAL_P(function), Z_STRLEN_P(function)+1);
		
		if (zend_hash_update(
			table, 
			lcname, Z_STRLEN_P(function)+1, 
			(void**) override, sizeof(zend_function), 
			(void**) overridden) == SUCCESS) {
			result = 1;
		}
		
		efree(lcname);
	}
	
	return result;
} /* }}} */

/* {{{ proto bool uopz_function(string function, Closure handler)
	   proto bool uopz_function(string class, string method, Closure handler [, bool static = false]) */
PHP_FUNCTION(uopz_function) {
	zval *function = NULL;
	zval *callable = NULL;
	HashTable *table = CG(function_table);
	zend_class_entry *clazz = NULL;
	zend_function *override = NULL;
	zend_function *method = NULL;
	zend_bool is_static = 0;
	
	switch (ZEND_NUM_ARGS()) {
		case 4:
		case 3: {
			if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "CzO|b", &clazz, &function, &callable, zend_ce_closure, &is_static) != SUCCESS) {
				return;
			} else {
				table = &clazz->function_table;
			}
		} break;
		
		default: {
			if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "zO", &function, &callable, zend_ce_closure) != SUCCESS) {
				return;
			}
		}
	}
	
	method = (zend_function*) 
		zend_get_closure_method_def(callable TSRMLS_CC);

	if (!uopz_function(table, function, method, &override TSRMLS_CC)) {
		RETURN_FALSE;
	}

	if (table != CG(function_table)) {
		uopz_magic_t *magic = umagic;

		while (magic && magic->name) {
			if (Z_STRLEN_P(function) == magic->length &&
				strncasecmp(Z_STRVAL_P(function), magic->name, magic->length) == SUCCESS) {

				switch (magic->id) {
					case 0: clazz->constructor = override; break;
					case 1: clazz->destructor = override; break;
					case 2: clazz->clone = override; break;
					case 3: clazz->__get = override; break;
					case 4: clazz->__set = override; break;
					case 5: clazz->__unset = override; break;
					case 6: clazz->__isset = override; break;
					case 7: clazz->__call = override; break;
					case 8: clazz->__callstatic = override; break;
					case 9: clazz->__tostring = override; break;
					case 10: clazz->serialize_func = override; break;
					case 11: clazz->unserialize_func = override; break;
#ifdef ZEND_DEBUGINFO_FUNC_NAME
					case 12: clazz->__debugInfo = override; break;
#endif
				}
			}
			magic++;
		}
	
		override->common.prototype = NULL;
		override->common.scope = clazz;
		
		if (is_static) {
			override->common.fn_flags |= ZEND_ACC_STATIC;
		} else override->common.fn_flags &= ~ ZEND_ACC_STATIC;
	}
	
	RETURN_TRUE;
} /* }}} */

static inline zend_bool uopz_implement(zend_class_entry *clazz, zend_class_entry *interface TSRMLS_DC) {
	zend_bool result = 0;
	zend_bool is_final = clazz->ce_flags;
	
	clazz->ce_flags &= ~ZEND_ACC_FINAL;

	if (!instanceof_function(clazz, interface TSRMLS_CC)) {
		if (interface->ce_flags & ZEND_ACC_INTERFACE) {
			zend_do_implement_interface(clazz, interface TSRMLS_CC);
			
			result = instanceof_function
				(clazz, interface TSRMLS_CC);
		}
	}
		
	if (is_final)
		clazz->ce_flags |= ZEND_ACC_FINAL;
		
	return result;
}

/* {{{ proto bool uopz_implement(string class, string interface) */
PHP_FUNCTION(uopz_implement)
{
	zend_class_entry *clazz = NULL;
	zend_class_entry *interface = NULL;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "CC", &clazz, &interface) != SUCCESS) {
		return;
	}
	
	RETURN_BOOL(uopz_implement(clazz, interface TSRMLS_CC));
} /* }}} */

static inline zend_bool uopz_extend(zend_class_entry *clazz, zend_class_entry *parent TSRMLS_DC) {
	zend_bool result = 0;
	zend_bool is_final = clazz->ce_flags;

	clazz->ce_flags &= ~ZEND_ACC_FINAL;

	if (!instanceof_function(clazz, parent TSRMLS_CC)) {
		if (!(parent->ce_flags & ZEND_ACC_INTERFACE)) {
			if (parent->ce_flags & ZEND_ACC_TRAIT) {
				zend_do_implement_trait(clazz, parent TSRMLS_CC);
			} else zend_do_inheritance(clazz, parent TSRMLS_CC);
			result = instanceof_function(clazz, parent TSRMLS_CC);
		}
	}
	
	if (parent->ce_flags & ZEND_ACC_TRAIT)
		zend_do_bind_traits(clazz TSRMLS_CC);
	
	if (is_final)
		clazz->ce_flags |= ZEND_ACC_FINAL;
	
	return result;
}

/* {{{ proto bool uopz_extend(string class, string parent) */
PHP_FUNCTION(uopz_extend)
{
	zend_class_entry *clazz = NULL;
	zend_class_entry *parent = NULL;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "CC", &clazz, &parent) != SUCCESS) {
		return;
	}
	
	RETURN_BOOL(uopz_extend(clazz, parent TSRMLS_CC));
} /* }}} */

static inline zend_bool uopz_compose(char *class_name, int class_name_len, HashTable *classes, zval *construct TSRMLS_DC) {
	zend_bool result = 0;
	zval **clazz = NULL;
	HashPosition position;
	zend_class_entry *entry = NULL;
	
	char *lc_class_name = zend_str_tolower_dup(class_name, class_name_len);
	zend_ulong lc_class_hash = zend_inline_hash_func(lc_class_name, class_name_len+1);
		
	if (!zend_hash_quick_exists(CG(class_table), lc_class_name, class_name_len+1, lc_class_hash)) {

		entry = (zend_class_entry*) emalloc(sizeof(zend_class_entry));
		entry->name = estrndup(class_name, class_name_len);
		entry->name_length = class_name_len;
		entry->type = ZEND_USER_CLASS;
		zend_initialize_class_data(entry, 1 TSRMLS_CC);

		if (zend_hash_quick_update(
			CG(class_table),
			lc_class_name, class_name_len+1, lc_class_hash,
			(void**)&entry, sizeof(zend_class_entry*), NULL) == SUCCESS) {
			
			for (zend_hash_internal_pointer_reset_ex(classes, &position);
				zend_hash_get_current_data_ex(classes, (void**)&clazz, &position) == SUCCESS;
				zend_hash_move_forward_ex(classes, &position)) {
				zend_class_entry **parent = NULL;

				if (Z_TYPE_PP(clazz) == IS_STRING) {
					if (zend_lookup_class(
							Z_STRVAL_PP(clazz),
							Z_STRLEN_PP(clazz), &parent TSRMLS_CC) == SUCCESS) {

						if ((*parent)->ce_flags & ZEND_ACC_INTERFACE) {
							zend_do_implement_interface(entry, *parent TSRMLS_CC);
						} else if ((*parent)->ce_flags & ZEND_ACC_TRAIT) {
							zend_do_implement_trait(entry, *parent TSRMLS_CC);
						} else zend_do_inheritance(entry, *parent TSRMLS_CC);
					}
				}
			}

			if (construct) {
				const zend_function *method = zend_get_closure_method_def(construct TSRMLS_CC);
			
				if (zend_hash_update(
						&entry->function_table,
						"__construct", sizeof("__construct")-1,
						(void**)method, sizeof(zend_function),
						(void**) &entry->constructor) == SUCCESS) {
					function_add_ref
						(entry->constructor);
					entry->constructor->common.scope = entry;
					entry->constructor->common.prototype = NULL;
				}
			}
		
			zend_do_bind_traits(entry TSRMLS_CC);	
			
			result = 1;	
		}
	}
	
	efree(lc_class_name);
	
	return result;
}

/* {{{ proto bool uopz_compose(string name, array classes [, Closure __construct]) */
PHP_FUNCTION(uopz_compose)
{
	char *class_name = NULL;
	int class_name_len = 0;
	char *lc_class_name = NULL;
	zend_ulong lc_class_hash = 0L;
	HashTable *classes = NULL;
	zval *construct = NULL;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sh|O", &class_name, &class_name_len, &classes, &construct, zend_ce_closure) != SUCCESS) {
		return;
	}

	RETURN_BOOL(uopz_compose(class_name, class_name_len, classes, construct TSRMLS_CC));
} /* }}} */

/* {{{ uopz */
ZEND_BEGIN_ARG_INFO(uopz_overload_arginfo, 1)
	ZEND_ARG_INFO(0, opcode)
	ZEND_ARG_INFO(0, callable)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(uopz_rename_arginfo, 2)
	ZEND_ARG_INFO(0, class)
	ZEND_ARG_INFO(0, function)
	ZEND_ARG_INFO(0, overload)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(uopz_backup_arginfo, 1)
	ZEND_ARG_INFO(0, class)
	ZEND_ARG_INFO(0, function)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(uopz_restore_arginfo, 1)
	ZEND_ARG_INFO(0, class)
	ZEND_ARG_INFO(0, function)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(uopz_delete_arginfo, 1)
	ZEND_ARG_INFO(0, class)
	ZEND_ARG_INFO(0, function)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(uopz_redefine_arginfo, 2)
	ZEND_ARG_INFO(0, class)
	ZEND_ARG_INFO(0, constant)
	ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(uopz_undefine_arginfo, 1)
	ZEND_ARG_INFO(0, class)
	ZEND_ARG_INFO(0, constant)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(uopz_function_arginfo, 2)
	ZEND_ARG_INFO(0, class)
	ZEND_ARG_INFO(0, function)
	ZEND_ARG_INFO(0, handler)
	ZEND_ARG_INFO(0, static)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(uopz_implement_arginfo, 2)
	ZEND_ARG_INFO(0, class)
	ZEND_ARG_INFO(0, interface)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(uopz_extend_arginfo, 2)
	ZEND_ARG_INFO(0, class)
	ZEND_ARG_INFO(0, parent)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(uopz_compose_arginfo, 2)
	ZEND_ARG_INFO(0, name)
	ZEND_ARG_INFO(0, classes)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(__uopz_exit_overload_arginfo, 0)
ZEND_END_ARG_INFO()
/* }}} */

/* {{{ uopz_functions[]
 */
static const zend_function_entry uopz_functions[] = {
	PHP_FE(uopz_overload, uopz_overload_arginfo)
	PHP_FE(uopz_backup, uopz_backup_arginfo)
	PHP_FE(uopz_restore, uopz_restore_arginfo)
	PHP_FE(uopz_rename, uopz_rename_arginfo)
	PHP_FE(uopz_delete, uopz_delete_arginfo)
	PHP_FE(uopz_redefine, uopz_redefine_arginfo)
	PHP_FE(uopz_undefine, uopz_undefine_arginfo)
	PHP_FE(uopz_function, uopz_function_arginfo)
	PHP_FE(uopz_implement, uopz_implement_arginfo)
	PHP_FE(uopz_extend, uopz_extend_arginfo)
	PHP_FE(uopz_compose, uopz_compose_arginfo)
	/* private function */
	PHP_FE(__uopz_exit_overload, __uopz_exit_overload_arginfo)
	{NULL, NULL, NULL}
};
/* }}} */

/* {{{ uopz_module_entry
 */
zend_module_entry uopz_module_entry = {
	STANDARD_MODULE_HEADER,
	PHP_UOPZ_EXTNAME,
	uopz_functions,
	PHP_MINIT(uopz),
	PHP_MSHUTDOWN(uopz),
	PHP_RINIT(uopz),
	PHP_RSHUTDOWN(uopz),
	PHP_MINFO(uopz),
	PHP_UOPZ_VERSION,
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

static int uopz_zend_startup(zend_extension *extension) /* {{{ */
{	
#if PHP_VERSION_ID >= 50700
	TSRMLS_FETCH();
	
	return zend_startup_module(&uopz_module_entry TSRMLS_CC);
#else
	return zend_startup_module(&uopz_module_entry);
#endif
}
/* }}} */

#ifndef ZEND_EXT_API
#define ZEND_EXT_API    ZEND_DLEXPORT
#endif
ZEND_EXTENSION();

ZEND_EXT_API zend_extension zend_extension_entry = {
	PHP_UOPZ_EXTNAME,
	PHP_UOPZ_VERSION,
	"Joe Watkins <krakjoe@php.net>",
	"https://github.com/krakjoe/uopz",
	"Copyright (c) 2014",
	uopz_zend_startup,
	NULL,           	    /* shutdown_func_t */
	NULL,                   /* activate_func_t */
	NULL,           	    /* deactivate_func_t */
	NULL,           	    /* message_handler_func_t */
	php_uopz_overload_exit, /* op_array_handler_func_t */
	NULL, 				    /* statement_handler_func_t */
	NULL,             	    /* fcall_begin_handler_func_t */
	NULL,           	    /* fcall_end_handler_func_t */
	NULL,      			    /* op_array_ctor_func_t */
	NULL,      			    /* op_array_dtor_func_t */
	STANDARD_ZEND_EXTENSION_PROPERTIES
};

#ifdef COMPILE_DL_UOPZ
ZEND_GET_MODULE(uopz)
#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
