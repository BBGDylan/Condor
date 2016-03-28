#include "parser.h"
#include "cobra/flags.h"

namespace Cobra{
namespace internal{

	Parser* Parser::New(Isolate* isolate, std::string* source){
		void* pt = isolate->GetMemory(sizeof(Parser));
		Parser* p = new(pt) Parser(isolate);
		p->SetDefaults(source);
		return p;
	}

	Parser::Parser(Isolate* iso){
		isolate = iso;
		expected = NULL;
		scanner = NULL;
		tok = NULL;
		pos = -1;
		row = -1;
		col = -1;
		reset = false;
		nonOp = false;
		isInline = false;
		isInternal = ALLOW_NATIVE;
		rootScope = Scope::New(isolate);
		scope = rootScope;
		printVariables = PRINT_VARIABLES;
		trace = TRACE_PARSER;
	}

	void Parser::SetDefaults(std::string* source){
		source = source;
		scanner = Scanner::New(isolate, source);
	}

	void Parser::SetRowCol(ASTNode* node){
		node->row = row;
		node->col = col;
	}

	void Parser::Parse(){
		try{
			Trace("\n\nParsing", "Started");
			ParseImportOrInclude();
			ParseShallowStmtList();
		}
		catch (Error::ERROR e){
			throw e;
		}
	}

	Scope* Parser::Parse(Isolate* iso, Scope* sc){
		if (sc == NULL) throw Error::INTERNAL_SCOPE_ERROR;
		if (sc->IsParsed()) return sc;
		Parser* p = Parser::New(iso, &sc->raw);
		p->Parse();
		iso->FreeMemory(sc, sizeof(Scope));
		Scope* result = p->GetBaseScope();
		iso->FreeMemory(p, sizeof(Parser));
		result->raw = sc->raw;
		result->SetParsed();
		return result;
	}

	void Parser::Trace(const char* name, const char* value){
		if (trace) printf("%s - %s\n", name, value);
	}

	void Parser::PrintTok(){
		printf("Debug: Raw: %s, Type: %s\n", tok->raw.c_str(), Token::ToString(tok->value));
	}

	void Parser::Next(){
		try {
			Row = row;
			Col = col;
			Pos = pos;
			if (tok != NULL) isolate->FreeMemory(tok, sizeof(Token));
			tok = scanner->NextToken();
			row = scanner->row;
			col = scanner->col;
			pos = scanner->offset;
		}
		catch (Error::ERROR e){
			throw e;
		}
	}

	void Parser::Expect(TOKEN val){
		if (tok == NULL || tok->value != val) {
			Token* t = Token::New(isolate, val);
			expected = t;
			throw Error::EXPECTED;
		}
	}

	bool Parser::Is(int argc, ...){
		va_list ap;
    va_start(ap, argc);
    bool isInList = false;
    for(int i = 1; i <= argc; i++) {
      if (!isInList) isInList = tok->value == va_arg(ap, int);
    }
    va_end(ap);
    return isInList;
	}

	bool Parser::IsOperator(){
		return Is(7, ADD, SUB, MOD, DIV, MUL, SHL, SHR);
	}

	bool Parser::IsAssignment(){
		return Is(12, ASSIGN, ADD_ASSIGN, SUB_ASSIGN, MUL_ASSIGN
								, DIV_ASSIGN, MOD_ASSIGN, AND_ASSIGN
								, OR_ASSIGN, XOR_ASSIGN, SHL_ASSIGN
								, SHR_ASSIGN, AND_NOT_ASSIGN);
	}

	bool Parser::IsVarType(){
		return Is(10, INT, BOOLEAN, FLOAT, DOUBLE, CHAR, STRING, IDENT, TRUE_LITERAL, FALSE_LITERAL, kNULL);
	}

	bool Parser::IsBoolean(){
		return Is(11, LAND, LOR, EQL, LSS, GTR, NEQ, LEQ, GEQ, NOT, XOR, AND);
	}

	/**
	 * Allowed syntax for include || import:
	 * 
	 * 		import "string"
	 *   	import "string" as "s"
	 *    import {
	 * 		 	"string",
	 * 		  "exception" as "e"
	 *    }
	 */
	void Parser::ParseImportOrInclude(){
		Next(); // first token
		bool isImport = false;
		bool group = false;
		if (!Is(2, IMPORT, INCLUDE)) return;
		Trace("Parsing", "Imports");
		while (true){
			isImport = Is(1, IMPORT);
			if (Is(2, IMPORT, INCLUDE)) {
				Next();
				if (Is(1, LBRACE)) {
					group = true;
					Next(); // eat {
				}
				else if (!Is(1, STRING)) throw Error::INVALID_INCLUDE_IMPORT;
			}
			else if (group && Is(1, COMMA)) Next(); // Eat ,
			else break;
			if (isImport) {
				ASTImport* import = ASTImport::New(isolate);
				SetRowCol(import);
				import->name = tok->raw;
				import->alias = ParseAlias();
				imports.push_back(import);
			}
			else {
				ASTInclude* include = ASTInclude::New(isolate);
				SetRowCol(include);
				include->name = tok->raw;
				include->alias = ParseAlias();
				includes.push_back(include);
			}
			if (group && !Is(2, COMMA, RBRACE)) throw Error::INVALID_INCLUDE_IMPORT;
		}
	}

	/**
	 * Allowed syntax:
	 * 
	 * 		as "string"
	 */
	std::string Parser::ParseAlias(){
		Next();
		if (Is(1, AS)){
			Next();
			Expect(STRING);
			std::string result = tok->raw;
			Next();
			return result;
		}
		return "";
	}

	void Parser::ParseShallowStmtList(TOKEN terminator){
		ASTNode* node = NULL;
		while (tok->value != terminator){
			int type = tok->Int();
			bool isExport = false;
			std::vector<TOKEN> visibility;
			node = NULL; // reset node
			switch (type){
				case EXPORT: {
					isExport = true;
					Next();
				}
				case PUBLIC: case STATIC: case PRIVATE: case PROTECTED: {
					if (isExport) throw Error::INVALID_USE_OF_EXPORT;
					while (Is(4, PUBLIC, STATIC, PRIVATE, PROTECTED)) {
						visibility.push_back(tok->value);
						Next();
					}
				}
				case FUNC: {
					node = ParseFunc(); 
					break;
				}
				case IDENT: {
					node = ParseIdentStart();
					break;
				}
				case INT: case STRING: case VAR: {
					std::vector<ASTVar*> list = ParseVarList();
					for (int i = 0; i < list.size(); i++){
						scope->Insert(list[i]);
					}
					break;
				}
				case FOR: {
					node = ParseForExpr();
					break;
				}
				case WHILE: {
					node = ParseWhile();
					break;
				}
				case TRY: {
					node = ParseTryCatch();
					break;
				}
				case THROW: {
					node = ParseThrow();
					break;
				}
				case IF: {
					node = ParseIf();
					break;
				}
				case DELETE: {
					node = ParseDelete();
					break;
				}
				case SWITCH: {
					node = ParseSwitch();
					break;
				}
				case OBJECT: {
					node = ParseObject();
					break;
				}
				case INTERNAL: {
					node = ParseInternal();
					break;
				}
				case RETURN: {
					node = ParseReturn();
					break;
				}
				default: {
					throw Error::UNEXPECTED_CHARACTER;
				}
			}
			if (node != NULL){
				node->isExport = isExport;
				node->visibility.insert(node->visibility.end(), visibility.begin(), visibility.end());
				scope->Insert(node);
			}
		}
	}

	/** 
	 * Allowed syntax:
	 *
	 * func name([type] ident, name){
	 * 		...
	 * }
	 */
	ASTFunc* Parser::ParseFunc(){
		Trace("Parsing", "Func");
		Expect(FUNC);
		Next();
		Expect(IDENT);
		ASTFunc* func = ASTFunc::New(isolate);
		SetRowCol(func);
		func->name = tok->raw;
		Next();
		std::vector<ASTVar*> vars = ParseFuncArgs();
		func->args.insert(func->args.end(), vars.begin(), vars.end());
		func->scope = LazyParseBody();
		return func;
	}

	Scope* Parser::LazyParseBody(){
		Trace("Storing", "Body for later");
		Expect(LBRACE);
		Next();
		int start = pos - tok->Length();
		int braceDepth = 1;
		while (true){
			if (Is(1, LBRACE)) braceDepth++;
			if (Is(1, RBRACE)) braceDepth--;
			if (braceDepth == 0) break;
			if (Is(1, END)) throw Error::UNEXPECTED_END_OF_FILE;
			Next(); // eat all tokens
		}
		int end = pos;
		Scope* scope = Scope::New(isolate);
		scope->raw = scanner->Substr(start, end);
		Next(); // eat }
		return scope;
	}

	// TODO
	ASTNode* Parser::ParseIdentStart(){
		Trace("Parsing", "Ident Start");
		ASTExpr* expr = ParseExpr();
		if (Is(1, SEMICOLON)) Next();
		return expr;
	}

	/**
	 * Allowed syntax:
	 *
	 * var a = 10;
	 * int a = 10;
	 * var {
	 * 		a = 10;
	 * 		b = 20;
	 * }
	 */
	std::vector<ASTVar*> Parser::ParseVarList(){
		std::vector<ASTVar*> vars;
		TOKEN base = tok->value;
		Next();
		bool isBrace = Is(1, LBRACE);
		if (Is(1, LBRACE)) Next();
		bool breakOut = false;
		bool isArray = Is(1, LBRACK);
		if (isArray){
			Next();
			Expect(RBRACK);
			Next();
		}
		while (true){
			if (Is(1, RBRACE)) {
				breakOut = true;
				break;
			}
			Expect(IDENT);
			ASTVar* var = ASTVar::New(isolate);
			SetRowCol(var);
			var->baseType = tok->value;
			var->name = tok->raw;
			var->isArray = isArray;
			Next();
			Trace("Parsing Var", var->name.c_str());
			var->assignmentType = tok->value;
			if (!IsAssignment() && !Is(1, SEMICOLON)) throw Error::INVALID_OPERATOR;
			Next();
			var->value = ParseExpr();
			if (var->value == NULL) var->value = ASTUndefined::New(isolate);
			vars.push_back(var);
			if (!isBrace) return vars;
		}
		if (breakOut) Next();
		return vars;
	}

	ASTExpr* Parser::ParseExpr(){
		Trace("Parsing", "Expression");
		ASTExpr* expr = ParseBinaryExpr();
		if (Is(1, SEMICOLON)) Next(); // loose semicolon
		return expr;
	}

	ASTExpr* Parser::ParseBinaryExpr(){
		Trace("Parsing", "Binary Expression");
		ASTExpr* expr = NULL;
		bool pre = true;
		TOKEN unary = UNDEFINED;
		bool incdec = Is(2, INC, DEC);
		if (incdec) {
			unary = tok->value;
			Next();
		}
		if (IsVarType()) expr = ParseVarType();
		if (Is(1, LPAREN)) expr = ParseFuncCall(expr);
		if (Is(2, INC, DEC) || incdec){
			ASTLiteral* lit = (ASTLiteral*) expr;
			if (!incdec) {
				lit->unary = tok->value;
				Next();
			}
			else {
				lit->unary = unary;
				lit->isPost = false;
			}
		}
		if (Is(1, LBRACK)){
			Trace("Parsing", "Array");
			ASTArray* ary = ASTArray::New(isolate);
			Next();
			while (!Is(1, RBRACK)){
				ary->members.push_back(ParseExpr());
				if (Is(1, COMMA)) Next();
			}
			Next();
			return ary;
		}
		if (IsOperator() || IsBoolean()) { // needs to be last to catch all lingering operators
			ASTBinaryExpr* binary = ASTBinaryExpr::New(isolate);
			SetRowCol(binary);
			binary->left = expr;
			binary->op = tok->value;
			Trace("Parsing Operator", Token::ToString(tok->value));
			Next();
			binary->right = ParseExpr();
			return binary;
		}
		return expr;
	}

	ASTExpr* Parser::ParseVarType(){
		ASTLiteral* lit = ASTLiteral::New(isolate);
		SetRowCol(lit);
		lit->value = tok->raw;
		if (tok->raw.length() > 0) Trace("Parsing Literal", tok->raw.c_str());
		else Trace("Parsing Literal", tok->String().c_str());
		lit->litType = tok->value;
		Next();
		return lit;
	}

	ASTExpr* Parser::ParseFuncCall(ASTExpr* expr){
		if (expr == NULL) throw Error::INVALID_FUNCTION_CALL;
		Next();
		ASTFuncCall* call = ASTFuncCall::New(isolate);
		SetRowCol(call);
		ASTLiteral* lit = (ASTLiteral*) expr;
		call->name = lit->value;
		isolate->FreeMemory(expr, sizeof(ASTExpr));
		while (true){
			call->params.push_back(ParseExpr());
			if (Is(1, RPAREN)) break;
			if (!Is(1, COMMA)) throw Error::EXPECTED_PARAMETER;
			Next();
		}
		Next(); // eat )
		return call;
	}

	ASTExpr* Parser::ParseForExpr(){
		ASTForExpr* expr = ASTForExpr::New(isolate);
		SetRowCol(expr);
		Next();
		Expect(LPAREN);
		Next();
		expr->var = ParseVarList()[0];
		expr->condition = ParseBoolean();
		expr->tick = ParseExpr();
		Expect(RPAREN);
		Next();
		expr->scope = LazyParseBody();
		return expr;
	}

	ASTExpr* Parser::ParseBoolean(){
		Trace("Parsing", "Boolean");
		ASTBinaryExpr* binary = (ASTBinaryExpr*) ParseBinaryExpr();
		SetRowCol(binary);
		binary->isBoolean = true;
		return binary;
	}

	ASTExpr* Parser::ParseWhile(){
		Trace("Parsing", "While");
		ASTWhileExpr* expr = ASTWhileExpr::New(isolate);
		SetRowCol(expr);
		Next();
		Expect(LPAREN);
		Next();
		expr->condition = ParseBoolean();
		Expect(RPAREN);
		Next();
		expr->scope = LazyParseBody();
		return expr;
	}

	ASTExpr* Parser::ParseTryCatch(){
		Trace("Parsing", "Try Catch");
		ASTTryCatchExpr* expr = ASTTryCatchExpr::New(isolate);
		SetRowCol(expr);
		Next();
		expr->tryScope = LazyParseBody();
		Expect(CATCH);
		Next();
		std::vector<ASTVar*> vars = ParseFuncArgs();
		expr->catchParams.insert(expr->catchParams.end(), vars.begin(), vars.end());
		expr->catchScope = LazyParseBody();
		return expr;
	}

	std::vector<ASTVar*> Parser::ParseFuncArgs(){
		Expect(LPAREN);
		Next();
		std::vector<ASTVar*> vars;
		while (true){
			if (!Is(3, IDENT, RPAREN, VAR) && !IsVarType()) throw Error::INVALID_ARGUMENT_TYPE;
			if (Is(2, IDENT, VAR) || IsVarType()){
				std::string first = tok->raw;
				TOKEN firstType = tok->value;
				if (first.length() == 0) first = tok->String();
				Next();
				std::string second = "";
				if (Is(1, IDENT)) {
					second = tok->raw;
					Next();
				}
				ASTVar* var = ASTVar::New(isolate);
				SetRowCol(var);
				var->name = second;
				var->baseName = first;
				var->baseType = firstType;
				vars.push_back(var);
			}
			if (Is(1, RPAREN)) break;
			Expect(COMMA);
			Next();
		}
		Next();
		return vars;
	}

	ASTExpr* Parser::ParseThrow(){
		Trace("Parsing", "Throw");
		Next();
		ASTThrow* stmt = ASTThrow::New(isolate);
		SetRowCol(stmt);
		stmt->expr = ParseExpr();
		return stmt;
	}

	ASTExpr* Parser::ParseIf(){
		Trace("Parsing", "If");
		ASTIf* stmt = ASTIf::New(isolate);
		SetRowCol(stmt);
		if (Is(1, ELSE)){
			Next();
			if (Is(1, LBRACE)){
				ASTLiteral* lit = ASTLiteral::New(isolate);
				SetRowCol(lit);
				lit->value = "true";
				lit->litType = TRUE_LITERAL;
				stmt->condition = lit;
				stmt->scope = LazyParseBody();
			}
		}
		if (Is(1, IF)){
			Next();
			Expect(LPAREN);
			Next();
			stmt->condition = ParseBoolean();
			Expect(RPAREN);
			Next();
			stmt->scope = LazyParseBody();
			while (Is(2, ELSE, IF)){
				ASTIf* ifs = (ASTIf*) ParseIf();
				stmt->elseIfs.push_back(ifs);
			}
			return stmt;
		}
		isolate->FreeMemory(stmt, sizeof(ASTIf));
		return NULL;
	}

	ASTNode* Parser::ParseDelete(){
		Trace("Parsing", "Delete");
		Next();
		Expect(IDENT);
		ASTDelete* del = ASTDelete::New(isolate);
		SetRowCol(del);
		del->node = ParseVarType();
		Expect(SEMICOLON);
		Next();
		return del;
	}

	ASTExpr* Parser::ParseSwitch(){
		Trace("Parsing", "Switch");
		Next();
		ASTSwitch* expr = ASTSwitch::New(isolate);
		SetRowCol(expr);
		Expect(LPAREN);
		Next();
		expr->value = ParseExpr();
		Expect(RPAREN);
		Next();
		Expect(LBRACE);
		Next();
		while (true){
			ASTCase* e = (ASTCase*) ParseCase();
			if (e == NULL) break;
			expr->cases.push_back(e);
		}
		Expect(RBRACE);
		Next();
		return expr;
	}

	ASTExpr* Parser::ParseCase(){
		ASTCase* expr = ASTCase::New(isolate);
		SetRowCol(expr);
		if (Is(1, CASE)){
			Trace("Parsing", "Case");
			Next();
			expr->condition = ParseVarType();
		}
		else if (Is(1, DEFAULT)){
			Trace("Parsing", "Default");
			Next();
		}
		else {
			isolate->FreeMemory(expr, sizeof(ASTCase));
			return NULL;
		}
		Expect(COLON);
		Next();
		expr->scope = LazyParseBody();
		return expr;
	}

	ASTNode* Parser::ParseObject(){
		Next();
		Expect(IDENT);
		Trace("Parsing Object", tok->raw.c_str());
		ASTObject* obj = ASTObject::New(isolate);
		SetRowCol(obj);
		Next();
		Expect(LBRACE);
		Next();
		obj->scope = Scope::New(isolate);
		OpenScope(obj->scope);
		ParseShallowStmtList(RBRACE);
		CloseScope();
		Expect(RBRACE);
		Next();
		return obj;
	}

	void Parser::OpenScope(Scope* sc){
		sc->outer = scope;
		scope = sc;
	}

	void Parser::CloseScope(){
		if (scope == NULL) throw Error::INTERNAL_SCOPE_ERROR;
		scope = scope->outer;
	}

	ASTNode* Parser::ParseInternal(){
		Trace("Parsing Internal Func Call", tok->raw.c_str());
		ASTExpr* expr = ASTExpr::New(isolate);
		SetRowCol(expr);
		expr->name = tok->raw;
		ASTFuncCall* call = (ASTFuncCall* ) ParseFuncCall(expr);
		if (Is(1, SEMICOLON)) Next();
		if (isInternal) call->isInternal = true;
		else throw Error::UNEXPECTED_CHARACTER;
		return call;
	}

	ASTNode* Parser::ParseReturn(){
		Trace("Parsing", "Return");
		ASTVar* var = ASTVar::New(isolate);
		SetRowCol(var);
		var->name = tok->String();
		Next();
		var->value = ParseExpr();
		if (Is(1, SEMICOLON)) Next();
		return var;
	}
} // namespace internal
}	// namespace Cobra