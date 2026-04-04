/* ast_print.c — indented tree dump of AstNode for debugging the pipeline. */
#include "ast.h"

#include <stdio.h>

static void ast_print_node(const AstNode *n, int indent);

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
        default:
            printf("(node kind %d)\n", (int)n->kind);
            break;
    }
}

void ast_print(const AstNode *node, int indent) {
    ast_print_node(node, indent);
}
