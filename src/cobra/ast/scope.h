#ifndef SCOPE_H_
#define SCOPE_H_

#include <string>
#include <map>
#include <vector>

#include "cobra/global.h"
#include "cobra/mem/isolate.h"
#include "cobra/ast/node.h"

namespace Cobra {
namespace internal{
	class ASTNode;
	class Type;

	class Scope
	{
	private:
		bool isParsed;
		std::vector<ASTNode*> nodes;
		
	public:
		Scope();
		~Scope();
		Scope* outer;
		std::string raw;
		static Scope* New(Isolate* isolate);
		void Insert(ASTNode* node);
		void InsertBefore(ASTNode* node);
		int Size();
		bool IsParsed(){return isParsed;}
		void SetParsed(){isParsed = true;}
		ASTNode* Get(int idx);
		std::vector<ASTNode*> Lookup(std::string name, bool deep = true);
	};
} // namespace internal
}

#endif // SCOPE_H_
