/******************************************************************************
* Icinga 2                                                                   *
* Copyright (C) 2012-2014 Icinga Development Team (http://www.icinga.org)    *
*                                                                            *
* This program is free software; you can redistribute it and/or              *
* modify it under the terms of the GNU General Public License                *
* as published by the Free Software Foundation; either version 2             *
* of the License, or (at your option) any later version.                     *
*                                                                            *
* This program is distributed in the hope that it will be useful,            *
* but WITHOUT ANY WARRANTY; without even the implied warranty of             *
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
* GNU General Public License for more details.                               *
*                                                                            *
* You should have received a copy of the GNU General Public License          *
* along with this program; if not, write to the Free Software Foundation     *
* Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.             *
******************************************************************************/

#ifndef VMOPS_H
#define VMOPS_H

#include "config/i2-config.hpp"
#include "config/expression.hpp"
#include "config/configitembuilder.hpp"
#include "config/applyrule.hpp"
#include "config/objectrule.hpp"
#include "base/debuginfo.hpp"
#include "base/array.hpp"
#include "base/dictionary.hpp"
#include "base/scriptfunction.hpp"
#include "base/scriptsignal.hpp"
#include "base/scriptvariable.hpp"
#include "base/configerror.hpp"
#include "base/convert.hpp"
#include "base/objectlock.hpp"
#include <boost/foreach.hpp>
#include <map>
#include <vector>

namespace icinga
{

class VMOps
{
public:
	static inline Value Variable(VMFrame& frame, const String& name)
	{
		if (name == "this")
			return frame.Self;

		if (frame.Locals && frame.Locals->Contains(name))
			return frame.Locals->Get(name);
		else if (frame.Locals != frame.Self && HasField(frame.Self, name))
			return GetField(frame.Self, name);
		else
			return ScriptVariable::Get(name);
	}

	static inline Value FunctionCall(VMFrame& frame, const Value& funcName, const std::vector<Value>& arguments)
	{
		ScriptFunction::Ptr func;

		if (funcName.IsObjectType<ScriptFunction>())
			func = funcName;
		else
			func = ScriptFunction::GetByName(funcName);

		if (!func)
			BOOST_THROW_EXCEPTION(ConfigError("Function '" + funcName + "' does not exist."));

		return func->Invoke(arguments);
	}

	static inline Value Indexer(VMFrame& frame, const std::vector<Expression *>& indexer)
	{
		Value result = indexer[0]->Evaluate(frame);

		for (int i = 1; i < indexer.size(); i++) {
			if (result.IsEmpty())
				return Empty;

			Value index = indexer[i]->Evaluate(frame);
			result = GetField(result, index);
		}

		return result;
	}

	static inline Value NewFunction(VMFrame& frame, const String& name, const std::vector<String>& args,
	    std::map<String, Expression *> *closedVars, const boost::shared_ptr<Expression>& expression)
	{
		ScriptFunction::Ptr func = new ScriptFunction(boost::bind(&FunctionWrapper, _1, args,
		    EvaluateClosedVars(frame, closedVars), expression));

		if (!name.IsEmpty())
			ScriptFunction::Register(name, func);

		return func;
	}

	static inline Value NewSlot(VMFrame& frame, const String& signal, const Value& slot)
	{
		ScriptSignal::Ptr sig = ScriptSignal::GetByName(signal);

		if (!sig)
			BOOST_THROW_EXCEPTION(ConfigError("Signal '" + signal + "' does not exist."));

		sig->AddSlot(boost::bind(SlotWrapper, slot, _1));

		return Empty;
	}

	static inline Value NewApply(VMFrame& frame, const String& type, const String& target, const String& name, const boost::shared_ptr<Expression>& filter,
		const String& fkvar, const String& fvvar, const boost::shared_ptr<Expression>& fterm, std::map<String, Expression *> *closedVars,
		const boost::shared_ptr<Expression>& expression, const DebugInfo& debugInfo = DebugInfo())
	{
		ApplyRule::AddRule(type, target, name, expression, filter, fkvar,
		    fvvar, fterm, debugInfo, EvaluateClosedVars(frame, closedVars));

		return Empty;
	}

	static inline Value NewObject(VMFrame& frame, bool abstract, const String& type, const String& name, const boost::shared_ptr<Expression>& filter,
		const String& zone, std::map<String, Expression *> *closedVars, const boost::shared_ptr<Expression>& expression, const DebugInfo& debugInfo = DebugInfo())
	{
		ConfigItemBuilder::Ptr item = new ConfigItemBuilder(debugInfo);

		String checkName = name;

		if (!abstract) {
			Type::Ptr ptype = Type::GetByName(type);

			NameComposer *nc = dynamic_cast<NameComposer *>(ptype.get());

			if (nc)
				checkName = nc->MakeName(name, Dictionary::Ptr());
		}

		if (!checkName.IsEmpty()) {
			ConfigItem::Ptr oldItem = ConfigItem::GetObject(type, checkName);

			if (oldItem) {
				std::ostringstream msgbuf;
				msgbuf << "Object '" << name << "' of type '" << type << "' re-defined: " << debugInfo << "; previous definition: " << oldItem->GetDebugInfo();
				BOOST_THROW_EXCEPTION(ConfigError(msgbuf.str()) << errinfo_debuginfo(debugInfo));
			}
		}

		item->SetType(type);

		if (name.FindFirstOf("!") != String::NPos) {
			std::ostringstream msgbuf;
			msgbuf << "Name for object '" << name << "' of type '" << type << "' is invalid: Object names may not contain '!'";
			BOOST_THROW_EXCEPTION(ConfigError(msgbuf.str()) << errinfo_debuginfo(debugInfo));
		}

		item->SetName(name);

		item->AddExpression(new OwnedExpression(expression));
		item->SetAbstract(abstract);
		item->SetScope(EvaluateClosedVars(frame, closedVars));
		item->SetZone(zone);
		item->SetFilter(filter);
		item->Compile()->Register();

		return Empty;
	}

	static inline Value For(VMFrame& frame, const String& fkvar, const String& fvvar, const Value& value, Expression *expression, const DebugInfo& debugInfo = DebugInfo())
	{
		if (value.IsObjectType<Array>()) {
			if (!fvvar.IsEmpty())
				BOOST_THROW_EXCEPTION(ConfigError("Cannot use dictionary iterator for array.") << errinfo_debuginfo(debugInfo));

			Array::Ptr arr = value;

			ObjectLock olock(arr);
			BOOST_FOREACH(const Value& value, arr) {
				frame.Locals->Set(fkvar, value);
				expression->Evaluate(frame);
			}
		} else if (value.IsObjectType<Dictionary>()) {
			if (fvvar.IsEmpty())
				BOOST_THROW_EXCEPTION(ConfigError("Cannot use array iterator for dictionary.") << errinfo_debuginfo(debugInfo));

			Dictionary::Ptr dict = value;

			ObjectLock olock(dict);
			BOOST_FOREACH(const Dictionary::Pair& kv, dict) {
				frame.Locals->Set(fkvar, kv.first);
				frame.Locals->Set(fvvar, kv.second);
				expression->Evaluate(frame);
			}
		}
		else
			BOOST_THROW_EXCEPTION(ConfigError("Invalid type in __for expression: " + value.GetTypeName()) << errinfo_debuginfo(debugInfo));

		return Empty;
	}

	static inline bool HasField(const Object::Ptr& context, const String& field)
	{
		Dictionary::Ptr dict = dynamic_pointer_cast<Dictionary>(context);

		if (dict)
			return dict->Contains(field);
		else {
			Type::Ptr type = context->GetReflectionType();

			if (!type)
				return false;

			return type->GetFieldId(field) != -1;
		}
	}

	static inline Value GetField(const Object::Ptr& context, const String& field)
	{
		Dictionary::Ptr dict = dynamic_pointer_cast<Dictionary>(context);

		if (dict)
			return dict->Get(field);

		Array::Ptr arr = dynamic_pointer_cast<Array>(context);

		if (arr) {
			int index = Convert::ToLong(field);
			return arr->Get(index);
		}

		Type::Ptr type = context->GetReflectionType();

		if (!type)
			return Empty;

		int fid = type->GetFieldId(field);

		if (fid == -1)
			return Empty;

		return context->GetField(fid);
	}

	static inline void SetField(const Object::Ptr& context, const String& field, const Value& value)
	{
		Dictionary::Ptr dict = dynamic_pointer_cast<Dictionary>(context);

		if (dict) {
			dict->Set(field, value);
			return;
		}

		Array::Ptr arr = dynamic_pointer_cast<Array>(context);

		if (arr) {
			int index = Convert::ToLong(field);
			if (index >= arr->GetLength())
				arr->Resize(index + 1);
			arr->Set(index, value);
			return;
		}

		Type::Ptr type = context->GetReflectionType();

		if (!type)
			BOOST_THROW_EXCEPTION(ConfigError("Cannot set field on object."));

		int fid = type->GetFieldId(field);

		if (fid == -1)
			BOOST_THROW_EXCEPTION(ConfigError("Attribute '" + field + "' does not exist."));

		try {
			context->SetField(fid, value);
		} catch (const boost::bad_lexical_cast&) {
			BOOST_THROW_EXCEPTION(ConfigError("Attribute '" + field + "' cannot be set to value of type '" + value.GetTypeName() + "'"));
		} catch (const std::bad_cast&) {
			BOOST_THROW_EXCEPTION(ConfigError("Attribute '" + field + "' cannot be set to value of type '" + value.GetTypeName() + "'"));
		}
	}

private:
	static inline Value FunctionWrapper(const std::vector<Value>& arguments,
	    const std::vector<String>& funcargs, const Dictionary::Ptr& closedVars, const boost::shared_ptr<Expression>& expr)
	{
		if (arguments.size() < funcargs.size())
			BOOST_THROW_EXCEPTION(ConfigError("Too few arguments for function"));

		VMFrame frame;

		if (closedVars)
			closedVars->CopyTo(frame.Locals);

		for (std::vector<Value>::size_type i = 0; i < std::min(arguments.size(), funcargs.size()); i++)
			frame.Locals->Set(funcargs[i], arguments[i]);

		Value result;
		try {
			result = expr->Evaluate(frame);
		} catch (const InterruptExecutionError& iee) {
			result = iee.GetResult();
		}

		return result;
	}

	static inline void SlotWrapper(const Value& funcName, const std::vector<Value>& arguments)
	{
		ScriptFunction::Ptr func;

		if (funcName.IsObjectType<ScriptFunction>())
			func = funcName;
		else
			func = ScriptFunction::GetByName(funcName);

		if (!func)
			BOOST_THROW_EXCEPTION(ConfigError("Function '" + funcName + "' does not exist."));

		func->Invoke(arguments);
	}

	static inline Dictionary::Ptr EvaluateClosedVars(VMFrame& frame, std::map<String, Expression *> *closedVars)
	{
		Dictionary::Ptr locals;

		if (closedVars) {
			locals = new Dictionary();

			typedef std::pair<String, Expression *> ClosedVar;
			BOOST_FOREACH(const ClosedVar& cvar, *closedVars) {
				locals->Set(cvar.first, cvar.second->Evaluate(frame));
			}
		}

		return locals;
	}
};

}

#endif /* VMOPS_H */
