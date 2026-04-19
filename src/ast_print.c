/* ast_print.c — indented tree dump of AstNode for debugging the pipeline. */
#include "ast.h"

#include <stdio.h>

static void ast_print_node(const AstNode *n, int indent);
static void ast_print_pattern(const AstNode *n, int indent);

static void print_indent(int indent) {
    int i;
    for (i = 0; i < indent; i++) {
        putchar(' ');
    }
}

static void print_list(const AstList *list, int indent, void (*fn)(const AstNode *, int)) {
    const AstList *l;
    for (l = list; l; l = l->next) {
        fn(l->item, indent);
    }
}

static void ast_print_pattern(const AstNode *n, int indent) {
    if (!n) {
        print_indent(indent);
        printf("(null pat)\n");
        return;
    }
    print_indent(indent);
    switch (n->kind) {
        case NODE_PAT_WILDCARD:
            printf("PAT_WILDCARD\n");
            break;
        case NODE_PAT_LITERAL:
            printf("PAT_LITERAL\n");
            ast_print_node(AS_PAT_LIT(n).lit, indent + 2);
            break;
        case NODE_PAT_BIND:
            printf("PAT_BIND %s%s\n", AS_PAT_BIND(n).is_mut ? "mut " : "", AS_PAT_BIND(n).name);
            break;
        case NODE_PAT_ENUM:
            printf("PAT_ENUM %s::%s\n", AS_PAT_ENUM(n).type_name, AS_PAT_ENUM(n).variant);
            print_list(AS_PAT_ENUM(n).fields, indent + 2, ast_print_pattern);
            break;
        case NODE_PAT_STRUCT:
            printf("PAT_STRUCT %s\n", AS_PAT_STRUCT(n).name);
            print_list(AS_PAT_STRUCT(n).field_pats, indent + 2, ast_print_pattern);
            break;
        case NODE_PAT_TUPLE:
            printf("PAT_TUPLE\n");
            print_list(AS_PAT_TUPLE(n).elements, indent + 2, ast_print_pattern);
            break;
        case NODE_PAT_ARRAY:
            printf("PAT_ARRAY\n");
            print_list(AS_PAT_ARRAY(n).elements, indent + 2, ast_print_pattern);
            break;
        case NODE_PAT_OR:
            printf("PAT_OR\n");
            ast_print_pattern(AS_PAT_OR(n).left, indent + 2);
            ast_print_pattern(AS_PAT_OR(n).right, indent + 2);
            break;
        case NODE_PAT_FIELD:
            printf("PAT_FIELD %s\n", AS_PAT_FIELD(n).field);
            if (AS_PAT_FIELD(n).pattern) {
                ast_print_pattern(AS_PAT_FIELD(n).pattern, indent + 2);
            }
            break;
        default:
            printf("(pattern kind %d)\n", (int)n->kind);
            break;
    }
}

static void ast_print_node(const AstNode *n, int indent) {
    if (!n) {
        print_indent(indent);
        printf("(null)\n");
        return;
    }

    print_indent(indent);

    switch (n->kind) {
        case NODE_PROGRAM:
            printf("PROGRAM\n");
            print_list(AS_PROGRAM(n).decls, indent + 2, ast_print_node);
            break;
        case NODE_FN_DECL:
            printf("FN %s%s\n", AS_FN_DECL(n).is_pub ? "pub " : "", AS_FN_DECL(n).name);
            print_list(AS_FN_DECL(n).params, indent + 2, ast_print_node);
            if (AS_FN_DECL(n).ret_type) {
                print_indent(indent + 2);
                printf("RET\n");
                ast_print_node(AS_FN_DECL(n).ret_type, indent + 4);
            }
            ast_print_node(AS_FN_DECL(n).body, indent + 2);
            break;
        case NODE_PARAM:
            printf("PARAM %s%s\n", AS_PARAM(n).is_mut ? "mut " : "", AS_PARAM(n).name);
            if (AS_PARAM(n).type) {
                ast_print_node(AS_PARAM(n).type, indent + 2);
            }
            break;
        case NODE_BLOCK:
            printf("BLOCK\n");
            print_list(AS_BLOCK(n).stmts, indent + 2, ast_print_node);
            if (AS_BLOCK(n).tail_expr) {
                print_indent(indent + 2);
                printf("TAIL\n");
                ast_print_node(AS_BLOCK(n).tail_expr, indent + 4);
            }
            break;
        case NODE_LET:
            printf("LET %s%s\n", AS_LET(n).is_mut ? "mut " : "", AS_LET(n).name);
            if (AS_LET(n).type) {
                ast_print_node(AS_LET(n).type, indent + 2);
            }
            ast_print_node(AS_LET(n).init, indent + 2);
            break;
        case NODE_EXPR_STMT:
            printf("EXPR_STMT\n");
            ast_print_node(AS_EXPR_STMT(n).expr, indent + 2);
            break;
        case NODE_RETURN:
            printf("RETURN\n");
            if (AS_RETURN(n).value) {
                ast_print_node(AS_RETURN(n).value, indent + 2);
            }
            break;
        case NODE_FOR:
            printf("FOR %s\n", AS_FOR(n).var);
            ast_print_node(AS_FOR(n).iter, indent + 2);
            ast_print_node(AS_FOR(n).body, indent + 2);
            break;
        case NODE_TRY:
            printf("TRY\n");
            ast_print_node(AS_TRY(n).body, indent + 2);
            print_list(AS_TRY(n).catch_clauses, indent + 2, ast_print_node);
            if (AS_TRY(n).finally_body) {
                print_indent(indent + 2);
                printf("FINALLY\n");
                ast_print_node(AS_TRY(n).finally_body, indent + 4);
            }
            break;
        case NODE_CATCH_CLAUSE:
            printf("CATCH %s\n", AS_CATCH(n).var);
            ast_print_node(AS_CATCH(n).type, indent + 2);
            ast_print_node(AS_CATCH(n).body, indent + 2);
            break;
        case NODE_THROW:
            printf("THROW\n");
            ast_print_node(AS_THROW(n).expr, indent + 2);
            break;
        case NODE_BINARY: {
            const char *op = "?";
            switch (AS_BINARY(n).op) {
                case BINOP_ADD: op = "+"; break;
                case BINOP_SUB: op = "-"; break;
                case BINOP_MUL: op = "*"; break;
                case BINOP_DIV: op = "/"; break;
                case BINOP_MOD: op = "%"; break;
                case BINOP_EQ: op = "=="; break;
                case BINOP_NEQ: op = "!="; break;
                case BINOP_LT: op = "<"; break;
                case BINOP_GT: op = ">"; break;
                case BINOP_LTE: op = "<="; break;
                case BINOP_GTE: op = ">="; break;
                case BINOP_AND: op = "&&"; break;
                case BINOP_OR: op = "||"; break;
            }
            printf("BINARY %s\n", op);
            ast_print_node(AS_BINARY(n).left, indent + 2);
            ast_print_node(AS_BINARY(n).right, indent + 2);
            break;
        }
        case NODE_UNARY: {
            const char *op = AS_UNARY(n).op == UNOP_NOT ? "!" : "-";
            printf("UNARY %s\n", op);
            ast_print_node(AS_UNARY(n).operand, indent + 2);
            break;
        }
        case NODE_CALL:
            printf("CALL\n");
            ast_print_node(AS_CALL(n).callee, indent + 2);
            if (AS_CALL(n).type_args) {
                print_indent(indent + 2);
                printf("TYPE_ARGS\n");
                print_list(AS_CALL(n).type_args, indent + 4, ast_print_node);
            }
            print_list(AS_CALL(n).args, indent + 2, ast_print_node);
            break;
        case NODE_IDENT:
            printf("IDENT %s\n", AS_IDENT(n).name);
            break;
        case NODE_LIT_INT:
            printf("INT %lld\n", (long long)AS_LIT_INT(n).value);
            break;
        case NODE_LIT_FLOAT:
            printf("FLOAT %f\n", (double)AS_LIT_FLOAT(n).value);
            break;
        case NODE_LIT_DOUBLE:
            printf("DOUBLE %f\n", AS_LIT_DOUBLE(n).value);
            break;
        case NODE_LIT_BOOL:
            printf("BOOL %s\n", AS_LIT_BOOL(n).value ? "true" : "false");
            break;
        case NODE_LIT_STRING:
            printf("STRING \"%s\"\n", AS_LIT_STRING(n).value);
            break;
        case NODE_LIT_NONE:
            printf("NONE\n");
            break;
        case NODE_TYPE_PRIMITIVE: {
            const char *name = "?";
            switch (AS_TYPE_PRIM(n).prim) {
                case PRIM_INT: name = "int"; break;
                case PRIM_FLOAT: name = "float"; break;
                case PRIM_DOUBLE: name = "double"; break;
                case PRIM_BOOL: name = "bool"; break;
                case PRIM_STRING: name = "string"; break;
            }
            printf("TYPE_PRIM %s\n", name);
            break;
        }
        case NODE_TYPE_NAMED:
            printf("TYPE_NAMED %s\n", AS_TYPE_NAMED(n).name);
            print_list(AS_TYPE_NAMED(n).type_args, indent + 2, ast_print_node);
            break;
        case NODE_TYPE_SELF:
            printf("TYPE_SELF\n");
            break;
        case NODE_TYPE_ARRAY:
            printf("TYPE_ARRAY\n");
            ast_print_node(AS_TYPE_ARRAY(n).elem_type, indent + 2);
            break;
        case NODE_IF:
            printf("IF\n");
            print_list(AS_IF(n).branches, indent + 2, ast_print_node);
            if (AS_IF(n).else_body) {
                print_indent(indent + 2);
                printf("ELSE\n");
                ast_print_node(AS_IF(n).else_body, indent + 4);
            }
            break;
        case NODE_MATCH:
            printf("MATCH\n");
            ast_print_node(AS_MATCH(n).subject, indent + 2);
            print_list(AS_MATCH(n).arms, indent + 2, ast_print_node);
            break;
        case NODE_MATCH_ARM:
            printf("MATCH_ARM\n");
            ast_print_pattern(AS_MATCH_ARM(n).pattern, indent + 2);
            if (AS_MATCH_ARM(n).guard) {
                print_indent(indent + 2);
                printf("GUARD\n");
                ast_print_node(AS_MATCH_ARM(n).guard, indent + 4);
            }
            ast_print_node(AS_MATCH_ARM(n).body, indent + 2);
            break;
        case NODE_STRUCT_DECL:
            printf("STRUCT %s%s\n", AS_STRUCT_DECL(n).is_pub ? "pub " : "", AS_STRUCT_DECL(n).name);
            print_list(AS_STRUCT_DECL(n).fields, indent + 2, ast_print_node);
            break;
        case NODE_STRUCT_FIELD:
            printf("FIELD %s%s%s\n",
                   AS_STRUCT_FIELD(n).is_pub ? "pub " : "",
                   AS_STRUCT_FIELD(n).is_mut ? "mut " : "",
                   AS_STRUCT_FIELD(n).name);
            ast_print_node(AS_STRUCT_FIELD(n).type, indent + 2);
            break;
        case NODE_IMPL_DECL:
            if (AS_IMPL_DECL(n).trait_name) {
                printf("IMPL %s%s for %s\n",
                       AS_IMPL_DECL(n).is_pub ? "pub " : "",
                       AS_IMPL_DECL(n).trait_name,
                       AS_IMPL_DECL(n).struct_name);
            } else {
                printf("IMPL %s%s\n",
                       AS_IMPL_DECL(n).is_pub ? "pub " : "",
                       AS_IMPL_DECL(n).struct_name);
            }
            print_list(AS_IMPL_DECL(n).methods, indent + 2, ast_print_node);
            break;
        case NODE_TRAIT_DECL:
            printf("TRAIT %s%s\n", AS_TRAIT_DECL(n).is_pub ? "pub " : "", AS_TRAIT_DECL(n).name);
            print_list(AS_TRAIT_DECL(n).items, indent + 2, ast_print_node);
            break;
        case NODE_TRAIT_FN_SIG:
            printf("TRAIT_FN_SIG %s\n", AS_TRAIT_FN_SIG(n).name);
            print_list(AS_TRAIT_FN_SIG(n).params, indent + 2, ast_print_node);
            if (AS_TRAIT_FN_SIG(n).ret_type) {
                print_indent(indent + 2);
                printf("RET\n");
                ast_print_node(AS_TRAIT_FN_SIG(n).ret_type, indent + 4);
            }
            break;
        case NODE_CONST_DECL:
            printf("CONST %s%s\n", AS_CONST_DECL(n).is_pub ? "pub " : "", AS_CONST_DECL(n).name);
            ast_print_node(AS_CONST_DECL(n).type, indent + 2);
            ast_print_node(AS_CONST_DECL(n).value, indent + 2);
            break;
        case NODE_LAMBDA:
            printf("LAMBDA\n");
            print_list(AS_LAMBDA(n).params, indent + 2, ast_print_node);
            if (AS_LAMBDA(n).ret_type) {
                print_indent(indent + 2);
                printf("RET\n");
                ast_print_node(AS_LAMBDA(n).ret_type, indent + 4);
            }
            ast_print_node(AS_LAMBDA(n).body, indent + 2);
            break;
        default:
            printf("(node kind %d)\n", (int)n->kind);
            break;
    }
}

void ast_print(const AstNode *node, int indent) {
    ast_print_node(node, indent);
}
