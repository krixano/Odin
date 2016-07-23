#include "../exact_value.cpp"
#include "entity.cpp"
#include "type.cpp"

enum AddressingMode {
	Addressing_Invalid,

	Addressing_NoValue,
	Addressing_Value,
	Addressing_Variable,
	Addressing_Constant,
	Addressing_Type,
	Addressing_Builtin,

	Addressing_Count,
};

struct Operand {
	AddressingMode mode;
	Type *type;
	ExactValue value;

	AstNode *expression;
	BuiltinProcedureId builtin_id;
};

struct TypeAndValue {
	AddressingMode mode;
	Type *type;
	ExactValue value;
};

struct DeclarationInfo {
	Scope *scope;

	Entity **entities;
	isize entity_count;

	AstNode *type_expr;
	AstNode *init_expr;
	AstNode *proc_decl; // AstNode_ProcedureDeclaration

	Map<b32> deps; // Key: Entity *
	i32 mark;
};


void init_declaration_info(DeclarationInfo *d, Scope *scope) {
	d->scope = scope;
	map_init(&d->deps, gb_heap_allocator());
}

DeclarationInfo *make_declaration_info(gbAllocator a, Scope *scope) {
	DeclarationInfo *d = gb_alloc_item(a, DeclarationInfo);
	init_declaration_info(d, scope);
	return d;
}

void destroy_declaration_info(DeclarationInfo *d) {
	map_destroy(&d->deps);
}

b32 has_init(DeclarationInfo *d) {
	if (d->init_expr != NULL)
		return true;
	if (d->proc_decl != NULL) {
		if (d->proc_decl->procedure_declaration.body != NULL)
			return true;
	}

	return false;
}


struct ExpressionInfo {
	b32 is_lhs; // Debug info
	AddressingMode mode;
	Type *type; // Type_Basic
	ExactValue value;
};

ExpressionInfo make_expression_info(b32 is_lhs, AddressingMode mode, Type *type, ExactValue value) {
	ExpressionInfo ei = {};
	ei.is_lhs = is_lhs;
	ei.mode   = mode;
	ei.type   = type;
	ei.value  = value;
	return ei;
}

struct ProcedureInfo {
	AstFile *file;
	Token            token;
	DeclarationInfo *decl;
	Type *           type; // Type_Procedure
	AstNode *        body; // AstNode_BlockStatement
};

struct Scope {
	Scope *parent;
	Scope *prev, *next;
	Scope *first_child, *last_child;
	Map<Entity *> elements; // Key: String
};

enum ExpressionKind {
	Expression_Expression,
	Expression_Conversion,
	Expression_Statement,
};

enum BuiltinProcedureId {
	BuiltinProcedure_Invalid,

	BuiltinProcedure_size_of,
	BuiltinProcedure_size_of_val,
	BuiltinProcedure_align_of,
	BuiltinProcedure_align_of_val,
	BuiltinProcedure_offset_of,
	BuiltinProcedure_offset_of_val,
	BuiltinProcedure_static_assert,
	BuiltinProcedure_len,
	BuiltinProcedure_cap,
	BuiltinProcedure_copy,
	BuiltinProcedure_copy_bytes,
	BuiltinProcedure_print,
	BuiltinProcedure_println,

	BuiltinProcedure_Count,
};
struct BuiltinProcedure {
	String name;
	isize arg_count;
	b32 variadic;
	ExpressionKind kind;
};
gb_global BuiltinProcedure builtin_procedures[BuiltinProcedure_Count] = {
	{STR_LIT(""),                 0, false, Expression_Statement},
	{STR_LIT("size_of"),          1, false, Expression_Expression},
	{STR_LIT("size_of_val"),      1, false, Expression_Expression},
	{STR_LIT("align_of"),         1, false, Expression_Expression},
	{STR_LIT("align_of_val"),     1, false, Expression_Expression},
	{STR_LIT("offset_of"),        2, false, Expression_Expression},
	{STR_LIT("offset_of_val"),    1, false, Expression_Expression},
	{STR_LIT("static_assert"),    1, false, Expression_Statement},
	{STR_LIT("len"),              1, false, Expression_Expression},
	{STR_LIT("cap"),              1, false, Expression_Expression},
	{STR_LIT("copy"),             2, false, Expression_Expression},
	{STR_LIT("copy_bytes"),       3, false, Expression_Statement},
	{STR_LIT("print"),            1, true,  Expression_Statement},
	{STR_LIT("println"),          1, true,  Expression_Statement},
};

struct CheckerContext {
	Scope *scope;
	DeclarationInfo *decl;
};

struct Checker {
	Parser *               parser;
	Map<TypeAndValue>      types;       // Key: AstNode * | Expression -> Type (and value)
	Map<Entity *>          definitions; // Key: AstNode * | Identifier -> Entity
	Map<Entity *>          uses;        // Key: AstNode * | Identifier -> Entity
	Map<Scope *>           scopes;      // Key: AstNode * | Node       -> Scope
	Map<ExpressionInfo>    untyped;     // Key: AstNode * | Expression -> ExpressionInfo
	Map<DeclarationInfo *> entities;    // Key: Entity *

	AstFile *              curr_ast_file;
	BaseTypeSizes          sizes;
	Scope *                global_scope;
	gbArray(ProcedureInfo) procedures; // NOTE(bill): Procedures to check

	gbArena     arena;
	gbAllocator allocator;

	CheckerContext context;

	gbArray(Type *) procedure_stack;
	b32 in_defer; // TODO(bill): Actually handle correctly

	ErrorCollector error_collector;
};

gb_global Scope *universal_scope = NULL;


Scope *make_scope(Scope *parent, gbAllocator allocator) {
	Scope *s = gb_alloc_item(allocator, Scope);
	s->parent = parent;
	map_init(&s->elements, gb_heap_allocator());
	if (parent != NULL && parent != universal_scope) {
		DLIST_APPEND(parent->first_child, parent->last_child, s);
	}
	return s;
}

void destroy_scope(Scope *scope) {
	isize element_count = gb_array_count(scope->elements.entries);
	for (isize i = 0; i < element_count; i++) {
		Entity *e =scope->elements.entries[i].value;
		if (e->kind == Entity_Variable) {
			if (!e->variable.used) {
				warning(e->token, "Unused variable `%.*s`", LIT(e->token.string));
			}
		}
	}

	for (Scope *child = scope->first_child; child != NULL; child = child->next) {
		destroy_scope(child);
	}

	map_destroy(&scope->elements);
	// NOTE(bill): No need to free scope as it "should" be allocated in an arena (except for the global scope)
}


void scope_lookup_parent_entity(Scope *s, String name, Scope **scope, Entity **entity) {
	u64 key = hash_string(name);
	for (; s != NULL; s = s->parent) {
		Entity **found = map_get(&s->elements, key);
		if (found) {
			if (entity) *entity = *found;
			if (scope) *scope = s;
			return;
		}
	}
	if (entity) *entity = NULL;
	if (scope) *scope = NULL;
}

Entity *scope_lookup_entity(Scope *s, String name) {
	Entity *entity = NULL;
	scope_lookup_parent_entity(s, name, NULL, &entity);
	return entity;
}

Entity *current_scope_lookup_entity(Scope *s, String name) {
	u64 key = hash_string(name);
	Entity **found = map_get(&s->elements, key);
	if (found)
		return *found;
	return NULL;
}



Entity *scope_insert_entity(Scope *s, Entity *entity) {
	String name = entity->token.string;
	u64 key = hash_string(name);
	Entity **found = map_get(&s->elements, key);
	if (found)
		return *found;
	map_set(&s->elements, key, entity);
	if (entity->parent == NULL)
		entity->parent = s;
	return NULL;
}

void add_dependency(DeclarationInfo *d, Entity *e) {
	map_set(&d->deps, hash_pointer(e), cast(b32)true);
}

void add_declaration_dependency(Checker *c, Entity *e) {
	if (c->context.decl) {
		auto found = map_get(&c->entities, hash_pointer(e));
		if (found) {
			add_dependency(c->context.decl, e);
		}
	}
}


void add_global_entity(Entity *entity) {
	String name = entity->token.string;
	if (gb_memchr(name.text, ' ', name.len)) {
		return; // NOTE(bill): `untyped thing`
	}
	if (scope_insert_entity(universal_scope, entity)) {
		GB_PANIC("Compiler error: double declaration");
	}
}

void add_global_constant(gbAllocator a, String name, Type *type, ExactValue value) {
	Token token = {Token_Identifier};
	token.string = name;
	Entity *entity = alloc_entity(a, Entity_Constant, NULL, token, type);
	entity->constant.value = value;
	add_global_entity(entity);
}

void init_universal_scope(void) {
	// NOTE(bill): No need to free these
	gbAllocator a = gb_heap_allocator();
	universal_scope = make_scope(NULL, a);

// Types
	for (isize i = 0; i < gb_count_of(basic_types); i++) {
		Token token = {Token_Identifier};
		token.string = basic_types[i].basic.name;
		add_global_entity(alloc_entity(a, Entity_TypeName, NULL, token, &basic_types[i]));
	}
	for (isize i = 0; i < gb_count_of(basic_type_aliases); i++) {
		Token token = {Token_Identifier};
		token.string = basic_type_aliases[i].basic.name;
		add_global_entity(alloc_entity(a, Entity_TypeName, NULL, token, &basic_type_aliases[i]));
	}

// Constants
	add_global_constant(a, make_string("true"),  &basic_types[Basic_UntypedBool],    make_exact_value_bool(true));
	add_global_constant(a, make_string("false"), &basic_types[Basic_UntypedBool],    make_exact_value_bool(false));
	add_global_constant(a, make_string("null"),  &basic_types[Basic_UntypedPointer], make_exact_value_pointer(NULL));

// Builtin Procedures
	for (isize i = 0; i < gb_count_of(builtin_procedures); i++) {
		BuiltinProcedureId id = cast(BuiltinProcedureId)i;
		Token token = {Token_Identifier};
		token.string = builtin_procedures[i].name;
		Entity *entity = alloc_entity(a, Entity_Builtin, NULL, token, &basic_types[Basic_Invalid]);
		entity->builtin.id = id;
		add_global_entity(entity);
	}
}







void init_checker(Checker *c, Parser *parser) {
	gbAllocator a = gb_heap_allocator();

	c->parser = parser;
	map_init(&c->types,       gb_heap_allocator());
	map_init(&c->definitions, gb_heap_allocator());
	map_init(&c->uses,        gb_heap_allocator());
	map_init(&c->scopes,      gb_heap_allocator());
	map_init(&c->entities,    gb_heap_allocator());
	c->sizes.word_size = 8;
	c->sizes.max_align = 8;

	map_init(&c->untyped, a);

	gb_array_init(c->procedure_stack, a);
	gb_array_init(c->procedures, a);

	// NOTE(bill): Is this big enough or too small?
	isize item_size = gb_max(gb_max(gb_size_of(Entity), gb_size_of(Type)), gb_size_of(Scope));
	isize total_token_count = 0;
	for (isize i = 0; i < gb_array_count(c->parser->files); i++) {
		AstFile *f = &c->parser->files[i];
		total_token_count += gb_array_count(f->tokens);
	}
	isize arena_size = 2 * item_size * total_token_count;
	gb_arena_init_from_allocator(&c->arena, a, arena_size);
	c->allocator = gb_arena_allocator(&c->arena);

	c->global_scope = make_scope(universal_scope, c->allocator);
	c->context.scope = c->global_scope;
}

void destroy_checker(Checker *c) {
	map_destroy(&c->types);
	map_destroy(&c->definitions);
	map_destroy(&c->uses);
	map_destroy(&c->scopes);
	map_destroy(&c->untyped);
	map_destroy(&c->entities);
	destroy_scope(c->global_scope);
	gb_array_free(c->procedure_stack);
	gb_array_free(c->procedures);

	gb_arena_free(&c->arena);
}


TypeAndValue *type_and_value_of_expression(Checker *c, AstNode *expression) {
	TypeAndValue *found = map_get(&c->types, hash_pointer(expression));
	return found;
}


Entity *entity_of_identifier(Checker *c, AstNode *identifier) {
	GB_ASSERT(identifier->kind == AstNode_Identifier);
	Entity **found = map_get(&c->definitions, hash_pointer(identifier));
	if (found)
		return *found;

	found = map_get(&c->uses, hash_pointer(identifier));
	if (found)
		return *found;
	return NULL;
}

Type *type_of_expression(Checker *c, AstNode *expression) {
	TypeAndValue *found = type_and_value_of_expression(c, expression);
	if (found)
		return found->type;
	if (expression->kind == AstNode_Identifier) {
		Entity *entity = entity_of_identifier(c, expression);
		if (entity)
			return entity->type;
	}

	return NULL;
}


void add_untyped(Checker *c, AstNode *expression, b32 lhs, AddressingMode mode, Type *basic_type, ExactValue value) {
	map_set(&c->untyped, hash_pointer(expression), make_expression_info(lhs, mode, basic_type, value));
}


void add_type_and_value(Checker *c, AstNode *expression, AddressingMode mode, Type *type, ExactValue value) {
	GB_ASSERT(expression != NULL);
	GB_ASSERT(type != NULL);
	if (mode == Addressing_Invalid)
		return;

	if (mode == Addressing_Constant) {
		GB_ASSERT(value.kind != ExactValue_Invalid);
		GB_ASSERT(type == &basic_types[Basic_Invalid] || is_type_constant_type(type));
	}

	TypeAndValue tv = {};
	tv.type = type;
	tv.value = value;
	map_set(&c->types, hash_pointer(expression), tv);
}

void add_entity_definition(Checker *c, AstNode *identifier, Entity *entity) {
	GB_ASSERT(identifier != NULL);
	GB_ASSERT(identifier->kind == AstNode_Identifier);
	u64 key = hash_pointer(identifier);
	map_set(&c->definitions, key, entity);
}

void add_entity(Checker *c, Scope *scope, AstNode *identifier, Entity *entity) {
	if (!are_strings_equal(entity->token.string, make_string("_"))) {
		Entity *insert_entity = scope_insert_entity(scope, entity);
		if (insert_entity) {
			error(&c->error_collector, entity->token, "Redeclared entity in this scope: %.*s", LIT(entity->token.string));
			return;
		}
	}
	if (identifier != NULL)
		add_entity_definition(c, identifier, entity);
}

void add_entity_use(Checker *c, AstNode *identifier, Entity *entity) {
	GB_ASSERT(identifier != NULL);
	GB_ASSERT(identifier->kind == AstNode_Identifier);
	u64 key = hash_pointer(identifier);
	map_set(&c->uses, key, entity);
}


void add_file_entity(Checker *c, AstNode *identifier, Entity *e, DeclarationInfo *d) {
	GB_ASSERT(are_strings_equal(identifier->identifier.token.string, e->token.string));


	add_entity(c, c->global_scope, identifier, e);
	map_set(&c->entities, hash_pointer(e), d);
	e->order = gb_array_count(c->entities.entries);
}


void check_procedure_later(Checker *c, AstFile *file, Token token, DeclarationInfo *decl, Type *type, AstNode *body) {
	ProcedureInfo info = {};
	info.file = file;
	info.token = token;
	info.decl  = decl;
	info.type  = type;
	info.body  = body;
	gb_array_append(c->procedures, info);
}



void add_scope(Checker *c, AstNode *node, Scope *scope) {
	GB_ASSERT(node != NULL);
	GB_ASSERT(scope != NULL);
	map_set(&c->scopes, hash_pointer(node), scope);
}


void check_open_scope(Checker *c, AstNode *statement) {
	Scope *scope = make_scope(c->context.scope, c->allocator);
	add_scope(c, statement, scope);
	c->context.scope = scope;
}

void check_close_scope(Checker *c) {
	c->context.scope = c->context.scope->parent;
}

void push_procedure(Checker *c, Type *procedure_type) {
	gb_array_append(c->procedure_stack, procedure_type);
}

void pop_procedure(Checker *c) {
	gb_array_pop(c->procedure_stack);
}


void add_curr_ast_file(Checker *c, AstFile *file) {
	gb_zero_item(&c->error_collector);
	c->curr_ast_file = file;
}





#include "expression.cpp"
#include "statements.cpp"





GB_COMPARE_PROC(entity_order_cmp) {
	Entity const *p = cast(Entity const *)a;
	Entity const *q = cast(Entity const *)b;
	return p->order < q->order ? -1 : p->order > q->order;
}

void check_parsed_files(Checker *c) {
	// Collect Entities
	for (isize i = 0; i < gb_array_count(c->parser->files); i++) {
		AstFile *f = &c->parser->files[i];
		add_curr_ast_file(c, f);
		for (AstNode *decl = f->declarations; decl != NULL; decl = decl->next) {
			if (!is_ast_node_declaration(decl))
				continue;

			switch (decl->kind) {
			case AstNode_BadDeclaration:
				break;

			case AstNode_VariableDeclaration: {
				auto *vd = &decl->variable_declaration;

				switch (vd->kind) {
				case Declaration_Immutable: {
					for (AstNode *name = vd->name_list, *value = vd->value_list;
					     name != NULL && value != NULL;
					     name = name->next, value = value->next) {
						GB_ASSERT(name->kind == AstNode_Identifier);
						ExactValue v = {ExactValue_Invalid};
						Entity *e = make_entity_constant(c->allocator, c->context.scope, name->identifier.token, NULL, v);
						DeclarationInfo *di = make_declaration_info(c->allocator, c->global_scope);
						di->type_expr = vd->type_expression;
						di->init_expr = value;
						add_file_entity(c, name, e, di);
					}

					isize lhs_count = vd->name_list_count;
					isize rhs_count = vd->value_list_count;

					if (rhs_count == 0 && vd->type_expression == NULL) {
						error(&c->error_collector, ast_node_token(decl), "Missing type or initial expression");
					} else if (lhs_count < rhs_count) {
						error(&c->error_collector, ast_node_token(decl), "Extra initial expression");
					}
				} break;

				case Declaration_Mutable: {
					isize entity_count = vd->name_list_count;
					isize entity_index = 0;
					Entity **entities = gb_alloc_array(c->allocator, Entity *, entity_count);
					DeclarationInfo *di = NULL;
					if (vd->value_list_count == 1) {
						di = make_declaration_info(gb_heap_allocator(), c->global_scope);
						di->entities = entities;
						di->entity_count = entity_count;
						di->type_expr = vd->type_expression;
						di->init_expr = vd->value_list;
					}

					AstNode *value = vd->value_list;
					for (AstNode *name = vd->name_list; name != NULL; name = name->next) {
						Entity *e = make_entity_variable(c->allocator, c->global_scope, name->identifier.token, NULL);
						entities[entity_index++] = e;

						DeclarationInfo *d = di;
						if (d == NULL) {
							AstNode *init_expr = value;
							d = make_declaration_info(gb_heap_allocator(), c->global_scope);
							d->type_expr = vd->type_expression;
							d->init_expr = init_expr;
						}

						add_file_entity(c, name, e, d);

						if (value != NULL)
							value = value->next;
					}
				} break;
				}


			} break;

			case AstNode_TypeDeclaration: {
				AstNode *identifier = decl->type_declaration.name;
				Entity *e = make_entity_type_name(c->allocator, c->global_scope, identifier->identifier.token, NULL);
				DeclarationInfo *d = make_declaration_info(c->allocator, e->parent);
				d->type_expr = decl->type_declaration.type_expression;
				add_file_entity(c, identifier, e, d);
			} break;

			case AstNode_ProcedureDeclaration: {
				AstNode *identifier = decl->procedure_declaration.name;
				Token token = identifier->identifier.token;
				Entity *e = make_entity_procedure(c->allocator, c->global_scope, token, NULL);
				add_entity(c, c->global_scope, identifier, e);
				DeclarationInfo *d = make_declaration_info(c->allocator, e->parent);
				d->proc_decl = decl;
				map_set(&c->entities, hash_pointer(e), d);
				e->order = gb_array_count(c->entities.entries);

			} break;

			case AstNode_ImportDeclaration:
				// NOTE(bill): ignore
				break;

			default:
				error(&c->error_collector, ast_node_token(decl), "Only declarations are allowed at file scope");
				break;
			}
		}
	}

	{ // Order entities
		gbArray(Entity *) entities;
		isize count = gb_array_count(c->entities.entries);
		gb_array_init_reserve(entities, gb_heap_allocator(), count);
		defer (gb_array_free(entities));

		for (isize i = 0; i < count; i++) {
			u64 key = c->entities.entries[i].key;
			Entity *e = cast(Entity *)cast(uintptr)key;
			gb_array_append(entities, e);
		}

		gb_sort_array(entities, count, entity_order_cmp);

		for (isize i = 0; i < count; i++) {
			check_entity_declaration(c, entities[i], NULL);
		}
	}


	// Check procedure bodies
	for (isize i = 0; i < gb_array_count(c->procedures); i++) {
		ProcedureInfo *pi = &c->procedures[i];
		add_curr_ast_file(c, pi->file);
		check_procedure_body(c, pi->token, pi->decl, pi->type, pi->body);
	}


	{ // Add untyped expression values
		isize count = gb_array_count(c->untyped.entries);
		for (isize i = 0; i < count; i++) {
			auto *entry = c->untyped.entries + i;
			u64 key = entry->key;
			AstNode *expr = cast(AstNode *)cast(uintptr)key;
			ExpressionInfo *info = &entry->value;
			if (is_type_typed(info->type)) {
				GB_PANIC("%s (type %s) is typed!", expression_to_string(expr), info->type);
			}
			add_type_and_value(c, expr, info->mode, info->type, info->value);
		}
	}
}



