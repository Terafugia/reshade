/**
 * Copyright (C) 2014 Patrick Mours. All rights reserved.
 * License: https://github.com/crosire/reshade#license
 */

#include "effect_parser.hpp"
#include "effect_codegen.hpp"
#include <assert.h>
#include <unordered_set>

using namespace reshadefx;

static inline uint32_t align(uint32_t address, uint32_t alignment)
{
	return (address % alignment != 0) ? address + alignment - address % alignment : address;
}

class codegen_glsl final : public codegen
{
public:
	codegen_glsl()
	{
		struct_info cbuffer_type;
		cbuffer_type.name = "$Globals";
		cbuffer_type.unique_name = "_Globals";
		cbuffer_type.definition = _cbuffer_type_id = make_id();
		_structs.push_back(cbuffer_type);

		_names[_cbuffer_type_id] = cbuffer_type.unique_name;
	}

private:
	id _next_id = 1;
	id _last_block = 0;
	id _current_block = 0;
	id _cbuffer_type_id = 0;
	std::unordered_map<id, std::string> _names;
	std::unordered_map<id, std::string> _blocks;
	unsigned int _scope_level = 0;
	unsigned int _current_cbuffer_offset = 0;
	unsigned int _current_sampler_binding = 0;
	std::unordered_map<id, std::vector<id>> _switch_fallthrough_blocks;
	std::vector<std::pair<std::string, bool>> _entry_points;

	inline id make_id() { return _next_id++; }

	inline std::string &code() { return _blocks[_current_block]; }

	void write_result(module &s) const override
	{
		if (_blocks.count(_cbuffer_type_id))
			s.hlsl += "layout(std140, binding = 0) uniform _Globals {\n" + _blocks.at(_cbuffer_type_id) + "};\n";
		s.hlsl += _blocks.at(0);

		s.samplers = _samplers;
		s.textures = _textures;
		s.uniforms = _uniforms;
		s.techniques = _techniques;
		s.entry_points = _entry_points;
	}

	std::string write_type(const type &type, bool is_param = false)
	{
		std::string s;

		if (type.has(type::q_precise))
			s += "precise ";

		if (is_param)
		{
			if (type.has(type::q_linear))
				s += "smooth ";
			if (type.has(type::q_noperspective))
				s += "noperspective ";
			if (type.has(type::q_centroid))
				s += "centroid ";
			if (type.has(type::q_nointerpolation))
				s += "flat ";

			if (type.has(type::q_inout))
				s += "inout ";
			else if (type.has(type::q_in))
				s += "in ";
			else if (type.has(type::q_out))
				s += "out ";
		}

		switch (type.base)
		{
		case type::t_void:
			s += "void";
			break;
		case type::t_bool:
			if (type.cols > 1)
				s += "mat" + std::to_string(type.rows) + 'x' + std::to_string(type.cols);
			else if (type.rows > 1)
				s += "bvec" + std::to_string(type.rows);
			else
				s += "bool";
			break;
		case type::t_int:
			if (type.cols > 1)
				s += "mat" + std::to_string(type.rows) + 'x' + std::to_string(type.cols);
			else if (type.rows > 1)
				s += "ivec" + std::to_string(type.rows);
			else
				s += "int";
			break;
		case type::t_uint:
			if (type.cols > 1)
				s += "mat" + std::to_string(type.rows) + 'x' + std::to_string(type.cols);
			else if (type.rows > 1)
				s += "uvec" + std::to_string(type.rows);
			else
				s += "uint";
			break;
		case type::t_float:
			if (type.cols > 1)
				s += "mat" + std::to_string(type.rows) + 'x' + std::to_string(type.cols);
			else if (type.rows > 1)
				s += "vec" + std::to_string(type.rows);
			else
				s += "float";
			break;
		case type::t_struct:
			s += id_to_name(type.definition);
			break;
		case type::t_sampler:
			s += "sampler2D";
			break;
		default:
			assert(false);
		}

		return s;
	}
	std::string write_constant(const type &type, const constant &data)
	{
		assert(type.is_numeric() || (type.is_struct() && data.as_uint[0] == 0));

		std::string s;

		if (type.is_array())
		{
			s += "{ ";

			struct type elem_type = type;
			elem_type.array_length = 0;

			for (size_t i = 0; i < data.array_data.size(); ++i)
				s += write_constant(elem_type, data.array_data[i]) + ", ";

			s += " }";
			return s;
		}

		if (!type.is_scalar())
		{
			if (type.is_matrix())
				s += "transpose";

			s += '(' + write_type(type);
		}

		s += '(';

		if (type.is_numeric())
		{
			for (unsigned int i = 0, components = type.components(); i < components; ++i)
			{
				switch (type.base)
				{
				case type::t_bool:
					s += data.as_uint[i] ? "true" : "false";
					break;
				case type::t_int:
					s += std::to_string(data.as_int[i]);
					break;
				case type::t_uint:
					s += std::to_string(data.as_uint[i]);
					break;
				case type::t_float:
					s += std::to_string(data.as_float[i]);
					break;
				}

				if (i < components - 1)
					s += ", ";
			}
		}

		if (!type.is_scalar())
		{
			s += ')';
		}

		s += ')';

		return s;
	}
	std::string write_scope()
	{
		return std::string(_scope_level, '\t');
	}
	std::string write_location(const location &loc)
	{
		return "#line " + std::to_string(loc.line) + '\n';
	}

	static std::string escape_name(const std::string &name)
	{
		std::string res;

		static const std::unordered_set<std::string> s_reserverd_names = {
			"common", "partition", "input", "ouput", "active", "filter", "superp", "invariant",
			"lowp", "mediump", "highp", "precision", "patch", "subroutine",
			"abs", "sign", "all", "any", "sin", "sinh", "cos", "cosh", "tan", "tanh", "asin", "acos", "atan",
			"exp", "exp2", "log", "log2", "sqrt", "inversesqrt", "ceil", "floor", "fract", "trunc", "round",
			"radians", "degrees", "length", "normalize", "transpose", "determinant", "intBitsToFloat", "uintBitsToFloat",
			"floatBitsToInt", "floatBitsToUint", "matrixCompMult", "not", "lessThan", "greaterThan", "lessThanEqual",
			"greaterThanEqual", "equal", "notEqual", "dot", "cross", "distance", "pow", "modf", "frexp", "ldexp",
			"min", "max", "step", "reflect", "texture", "textureOffset", "fma", "mix", "clamp", "smoothstep", "refract",
			"faceforward", "textureLod", "textureLodOffset", "texelFetch", "main"
		};

		if (name.compare(0, 3, "gl_") == 0 || s_reserverd_names.count(name))
		{
			res += '_';
		}

		res += name;

		size_t p;

		while ((p = res.find("__")) != std::string::npos)
		{
			res.replace(p, 2, "_US");
		}

		return res;
	}

	inline std::string id_to_name(id id) const
	{
		if (const auto it = _names.find(id); it != _names.end())
			return it->second;
		return '_' + std::to_string(id);
	}

	id   define_struct(const location &loc, struct_info &info) override
	{
		info.definition = make_id();
		_names[info.definition] = info.unique_name;

		_structs.push_back(info);

		code() += write_location(loc) + "struct " + info.unique_name + "\n{\n";

		for (const auto &member : info.member_list)
			code() += '\t' + write_type(member.type, true) + ' ' + member.name + ";\n";

		if (info.member_list.empty())
			code() += "float _dummy;\n";

		code() += "};\n";

		return info.definition;
	}
	id   define_texture(const location &, texture_info &info) override
	{
		info.id = make_id();

		_textures.push_back(info);

		return info.id;
	}
	id   define_sampler(const location &loc, sampler_info &info) override
	{
		info.id = make_id();
		info.binding = _current_sampler_binding++;

		_samplers.push_back(info);

		code() += write_location(loc) + "layout(binding = " + std::to_string(info.binding) + ") uniform sampler2D " + info.unique_name + ";\n";

		_names[info.id] = info.unique_name;

		return info.id;
	}
	id   define_uniform(const location &loc, uniform_info &info) override
	{
		const unsigned int size = 4 * (info.type.rows == 3 ? 4 : info.type.rows) * info.type.cols * std::max(1, info.type.array_length);
		const unsigned int alignment = size;
		info.size = size;
		info.offset = align(_current_cbuffer_offset, alignment);
		_current_cbuffer_offset = info.offset + info.size;

		struct_member_info member;
		member.type = info.type;
		member.name = info.name;

		const_cast<struct_info &>(find_struct(_cbuffer_type_id))
			.member_list.push_back(std::move(member));

		_blocks[_cbuffer_type_id] += write_location(loc) + write_type(info.type) + " _Globals_" + info.name + ";\n";

		info.member_index = static_cast<uint32_t>(_uniforms.size());

		_uniforms.push_back(info);

		return _cbuffer_type_id;
	}
	id   define_variable(const location &loc, const type &type, const char *name, bool, id initializer_value) override
	{
		const id res = make_id();

		if (name != nullptr)
			_names[res] = name;

		code() += write_location(loc) + write_scope() + write_type(type) + ' ' + id_to_name(res);

		if (type.is_array())
			code() += '[' + std::to_string(type.array_length) + ']';

		if (initializer_value != 0)
			code() += " = " + id_to_name(initializer_value);

		code() += ";\n";

		return res;
	}
	id   define_function(const location &loc, function_info &info) override
	{
		info.definition = make_id();
		_names[info.definition] = info.unique_name;

		code() += write_location(loc) + write_type(info.return_type) + ' ' + info.unique_name + '(';

		for (size_t i = 0, num_params = info.parameter_list.size(); i < num_params; ++i)
		{
			auto &param = info.parameter_list[i];

			param.definition = make_id();
			_names[param.definition] = param.name;

			code() += '\n' + write_location(param.location) + '\t' + write_type(param.type, true) + ' ' + param.name;

			if (i < num_params - 1)
				code() += ',';
		}

		code() += ")\n";

		_scope_level++;

		_functions.push_back(std::make_unique<function_info>(info));

		return info.definition;
	}

	id   create_block() override
	{
		return make_id();
	}

	void create_entry_point(const function_info &func, bool is_ps) override
	{
		if (const auto it = std::find_if(_entry_points.begin(), _entry_points.end(),
			[&func](const auto &ep) { return ep.first == func.unique_name; }); it != _entry_points.end())
			return;

		code() += "#ifdef ENTRY_POINT_" + func.unique_name + '\n';



		code() += "void main()\n{\n";

		_scope_level++;



		_scope_level--;

		code() += "}\n";

		code() += "#endif\n";

		_entry_points.push_back({ func.unique_name, is_ps });
	}

	id   emit_load(const expression &chain) override
	{
		const id res = make_id();

		code() += write_location(chain.location) + write_scope() + "const " + write_type(chain.type) + ' ' + id_to_name(res);

		if (chain.type.is_array())
			code() += '[' + std::to_string(chain.type.array_length) + ']';

		code() += " = ";

		if (chain.is_constant)
		{
			code() += write_constant(chain.type, chain.constant);
		}
		else
		{
			std::string newcode = id_to_name(chain.base);

			for (const auto &op : chain.ops)
			{
				switch (op.type)
				{
				case expression::operation::op_cast: {
					newcode = "((" + write_type(op.to) + ')' + newcode + ')';
					break;
				}
				case expression::operation::op_index:
					newcode += '[' + id_to_name(op.index) + ']';
					break;
				case expression::operation::op_member:
					newcode += op.from.definition == _cbuffer_type_id ? '_' : '.';
					newcode += find_struct(op.from.definition).member_list[op.index].name;
					break;
				case expression::operation::op_swizzle:
					newcode += '.';
					for (unsigned int i = 0; i < 4 && op.swizzle[i] >= 0; ++i)
						newcode += "xyzw"[op.swizzle[i]];
					break;
				}
			}

			code() += newcode;
		}

		code() += ";\n";

		return res;
	}
	void emit_store(const expression &chain, id value, const type &) override
	{
		code() += write_location(chain.location) + write_scope() + id_to_name(chain.base);

		for (const auto &op : chain.ops)
		{
			switch (op.type)
			{
			case expression::operation::op_index:
				code() += '[' + id_to_name(op.index) + ']';
				break;
			case expression::operation::op_member:
				code() += '.';
				code() += find_struct(op.from.definition).member_list[op.index].name;
				break;
			case expression::operation::op_swizzle:
				code() += '.';
				for (unsigned int i = 0; i < 4 && op.swizzle[i] >= 0; ++i)
					code() += "xyzw"[op.swizzle[i]];
				break;
			}
		}

		code() += " = " + id_to_name(value) + ";\n";
	}

	id   emit_constant(const type &type, const constant &data) override
	{
		assert(type.is_numeric());

		const id res = make_id();

		code() += write_scope() + "const " + write_type(type) + ' ' + id_to_name(res);

		if (type.is_array())
			code() += '[' + std::to_string(type.array_length) + ']';

		code() += " = " + write_constant(type, data) + ";\n";

		return res;
	}

	id   emit_unary_op(const location &loc, tokenid op, const type &res_type, id val) override
	{
		const id res = make_id();

		code() += write_location(loc) + write_scope() + "const " + write_type(res_type) + ' ' + id_to_name(res) + " = ";

		switch (op)
		{
		case tokenid::minus:
			code() += '-';
			break;
		case tokenid::tilde:
			code() += '~';
			break;
		case tokenid::exclaim:
			if (res_type.is_vector())
				code() += "not";
			else
				code() += "!bool";
			break;
		default:
			assert(false);
		}

		code() += "(" + id_to_name(val) + ");\n";

		return res;
	}
	id   emit_binary_op(const location &loc, tokenid op, const type &res_type, const type &type, id lhs, id rhs) override
	{
		const id res = make_id();

		code() += write_location(loc) + write_scope() + "const " + write_type(res_type) + ' ' + id_to_name(res) + " = ";

		std::string intrinsic, operator_code;

		switch (op)
		{
		case tokenid::plus:
		case tokenid::plus_plus:
		case tokenid::plus_equal:
			operator_code = '+';
			break;
		case tokenid::minus:
		case tokenid::minus_minus:
		case tokenid::minus_equal:
			operator_code = '-';
			break;
		case tokenid::star:
		case tokenid::star_equal:
			if (type.is_matrix())
				intrinsic = "matrixCompMult";
			else
				operator_code = '*';
			break;
		case tokenid::slash:
		case tokenid::slash_equal:
			operator_code = '/';
			break;
		case tokenid::percent:
		case tokenid::percent_equal:
			if (type.is_floating_point())
				intrinsic = "_fmod";
			else
				operator_code = '%';
			break;
		case tokenid::caret:
		case tokenid::caret_equal:
			operator_code = '^';
			break;
		case tokenid::pipe:
		case tokenid::pipe_equal:
			operator_code = '|';
			break;
		case tokenid::ampersand:
		case tokenid::ampersand_equal:
			operator_code = '&';
			break;
		case tokenid::less_less:
		case tokenid::less_less_equal:
			operator_code = "<<";
			break;
		case tokenid::greater_greater:
		case tokenid::greater_greater_equal:
			operator_code = ">>";
			break;
		case tokenid::pipe_pipe:
			operator_code = "||";
			break;
		case tokenid::ampersand_ampersand:
			operator_code = "&&";
			break;
		case tokenid::less:
			if (type.is_vector())
				intrinsic = "lessThan";
			else
				operator_code = '<';
			break;
		case tokenid::less_equal:
			if (type.is_vector())
				intrinsic = "lessThanEqual";
			else
				operator_code = "<=";
			break;
		case tokenid::greater:
			if (type.is_vector())
				intrinsic = "greaterThan";
			else
				operator_code = '>';
			break;
		case tokenid::greater_equal:
			if (type.is_vector())
				intrinsic = "greaterThanEqual";
			else
				operator_code = ">=";
			break;
		case tokenid::equal_equal:
			if (type.is_vector())
				intrinsic = "equal";
			else
				operator_code = "==";
			break;
		case tokenid::exclaim_equal:
			if (type.is_vector())
				intrinsic = "notEqual";
			else
				operator_code = "!=";
			break;
		default:
			assert(false);
		}

		if (!intrinsic.empty())
			code() += intrinsic + '(' + id_to_name(lhs) + ", " + id_to_name(rhs) + ')';
		else
			code() += id_to_name(lhs) + ' ' + operator_code + ' ' + id_to_name(rhs);

		code() += ";\n";

		return res;
	}
	id   emit_ternary_op(const location &loc, tokenid op, const type &res_type, id condition, id true_value, id false_value) override
	{
		assert(op == tokenid::question);

		const id res = make_id();

		code() += write_location(loc) + write_scope() + "const " + write_type(res_type) + ' ' + id_to_name(res);

		if (res_type.is_array())
			code() += '[' + std::to_string(res_type.array_length) + ']';

		code() += " = " + id_to_name(condition) + " ? " + id_to_name(true_value) + " : " + id_to_name(false_value) + ";\n";

		return res;
	}
	id   emit_call(const location &loc, id function, const type &res_type, const std::vector<expression> &args) override
	{
		for (const auto &arg : args)
			assert(arg.ops.empty() && arg.base != 0);

		const id res = make_id();

		code() += write_location(loc) + write_scope();

		if (!res_type.is_void())
		{
			code() += "const " + write_type(res_type) + ' ' + id_to_name(res);

			if (res_type.is_array())
				code() += '[' + std::to_string(res_type.array_length) + ']';

			code() += " = ";
		}

		code() += id_to_name(function) + '(';

		for (size_t i = 0, num_args = args.size(); i < num_args; ++i)
		{
			code() += id_to_name(args[i].base);

			if (i < num_args - 1)
				code() += ", ";
		}

		code() += ");\n";

		return res;
	}
	id   emit_call_intrinsic(const location &loc, id intrinsic, const type &res_type, const std::vector<expression> &args) override
	{
		for (const auto &arg : args)
			assert(arg.ops.empty() && arg.base != 0);

		const id res = make_id();

		code() += write_location(loc) + write_scope();

		if (!res_type.is_void())
		{
			code() += "const " + write_type(res_type) + ' ' + id_to_name(res) + " = ";
		}

		enum
		{
#define IMPLEMENT_INTRINSIC_GLSL(name, i, code) name##i,
#include "effect_symbol_table_intrinsics.inl"
		};

		switch (intrinsic)
		{
#define IMPLEMENT_INTRINSIC_GLSL(name, i, code) case name##i: code break;
#include "effect_symbol_table_intrinsics.inl"
		default:
			assert(false);
		}

		code() += ";\n";

		return res;
	}
	id   emit_construct(const location &loc, const type &type, const std::vector<expression> &args) override
	{
		for (const auto &arg : args)
			assert((arg.type.is_scalar() || type.is_array()) && arg.ops.empty() && arg.base != 0);

		const id res = make_id();

		code() += write_location(loc) + write_scope() + "const " + write_type(type) + ' ' + id_to_name(res);

		if (type.is_array())
			code() += '[' + std::to_string(type.array_length) + ']';

		code() += " = ";

		if (!type.is_array() && type.is_matrix())
			code() += "transpose(";

		code() += write_type(type);

		if (type.is_array())
			code() += "[]";

		code() += '(';

		for (size_t i = 0, num_args = args.size(); i < num_args; ++i)
		{
			code() += id_to_name(args[i].base);

			if (i < num_args - 1)
				code() += ", ";
		}

		if (!type.is_array() && type.is_matrix())
			code() += ')';

		code() += ");\n";

		return res;
	}

	void emit_if(const location &loc, id condition_value, id condition_block, id true_statement_block, id false_statement_block, unsigned int flags) override
	{
		assert(condition_value != 0 && condition_block != 0 && true_statement_block != 0 && false_statement_block != 0);

		code() += _blocks[condition_block];
		code() += write_location(loc);

		if (flags & flatten) code() += "[flatten]";
		if (flags & dont_flatten) code() += "[branch]";

		code() +=
			write_scope() + "if (" + id_to_name(condition_value) + ")\n" + write_scope() + "{\n" +
			_blocks[true_statement_block] +
			write_scope() + "}\n" + write_scope() + "else\n" + write_scope () + "{\n" +
			_blocks[false_statement_block] +
			write_scope() + "}\n";
	}
	id   emit_phi(const location &loc, id condition_value, id condition_block, id true_value, id true_statement_block, id false_value, id false_statement_block, const type &type) override
	{
		assert(condition_value != 0 && condition_block != 0 && true_value != 0 && true_statement_block != 0 && false_value != 0 && false_statement_block != 0);

		const id res = make_id();

		code() += _blocks[condition_block] +
			write_scope() + write_type(type) + ' ' + id_to_name(res) + ";\n" +
			write_location(loc) +
			write_scope() + "if (" + id_to_name(condition_value) + ")\n" + write_scope () + "{\n" +
			(true_statement_block != condition_block ? _blocks[true_statement_block] : std::string()) +
			write_scope() + id_to_name(res) + " = " + id_to_name(true_value) + ";\n" +
			write_scope() + "}\n" + write_scope() + "else\n" + write_scope () + "{\n" +
			(false_statement_block != condition_block ? _blocks[false_statement_block] : std::string()) +
			write_scope() + id_to_name(res) + " = " + id_to_name(false_value) + ";\n" +
			write_scope() + "}\n";

		return res;
	}
	void emit_loop(const location &loc, id condition_value, id prev_block, id, id condition_block, id loop_block, id continue_block, unsigned int flags) override
	{
		assert(condition_value != 0 && prev_block != 0 && loop_block != 0 && continue_block != 0);

		code() += _blocks[prev_block];

		if (condition_block == 0)
		{
			code() += write_scope() + "bool " + id_to_name(condition_value) + ";\n";
		}
		else
		{
			// Remove 'const' from condition variable
			std::string loop_condition = _blocks[condition_block];
			auto pos_assign = loop_condition.rfind(id_to_name(condition_value));
			auto pos_const_keyword = loop_condition.rfind("const", pos_assign);
			loop_condition.erase(pos_const_keyword, 6);

			code() += loop_condition;
		}

		code() += write_location(loc) + write_scope();

		if (flags & unroll) code() += "[unroll] ";
		if (flags & dont_unroll) code() += "[loop] ";

		if (condition_block == 0)
		{
			// Convert variable initializer to assignment statement
			std::string loop_condition = _blocks[continue_block];
			auto pos_assign = loop_condition.rfind(id_to_name(condition_value));
			auto pos_prev_assign = loop_condition.rfind('\n', pos_assign);
			loop_condition.erase(pos_prev_assign + 1, pos_assign - pos_prev_assign - 1);

			code() += "do\n" + write_scope() + "{\n" + _blocks[loop_block] + loop_condition + "}\n" + write_scope() + "while (" + id_to_name(condition_value) + ");\n";
		}
		else
		{
			// Convert variable initializer to assignment statement
			std::string loop_condition = _blocks[condition_block];
			auto pos_assign = loop_condition.rfind(id_to_name(condition_value));
			auto pos_prev_assign = loop_condition.rfind('\n', pos_assign);
			loop_condition.erase(pos_prev_assign + 1, pos_assign - pos_prev_assign - 1);

			code() += "while (" + id_to_name(condition_value) + ")\n" + write_scope() + "{\n" + _blocks[loop_block] + _blocks[continue_block] + loop_condition + write_scope() + "}\n";
		}
	}
	void emit_switch(const location &loc, id selector_value, id selector_block, id default_label, const std::vector<id> &case_literal_and_labels, unsigned int flags) override
	{
		assert(selector_value != 0 && selector_block != 0 && default_label != 0);

		code() += _blocks[selector_block];
		code() += write_location(loc) + write_scope();

		if (flags & flatten) code() += "[flatten]";
		if (flags & dont_flatten) code() += "[branch]";

		code() += "switch (" + id_to_name(selector_value) + ")\n" + write_scope() + "{\n";

		_scope_level++;

		for (size_t i = 0; i < case_literal_and_labels.size(); i += 2)
		{
			assert(case_literal_and_labels[i + 1] != 0);

			code() += write_scope() + "case " + std::to_string(case_literal_and_labels[i]) + ": {\n" + _blocks[case_literal_and_labels[i + 1]];

			// Handle fall-through blocks
			for (id fallthrough : _switch_fallthrough_blocks[case_literal_and_labels[i + 1]])
				code() += _blocks[fallthrough];

			code() += write_scope() + "}\n";
		}

		if (default_label != _current_block)
		{
			code() += write_scope() + "default: {\n" + _blocks[default_label] + write_scope() + "}\n";
		}

		_scope_level--;

		code() += write_scope() + "}\n";
	}

	bool is_in_block() const override { return _current_block != 0; }
	bool is_in_function() const override { return _scope_level > 0; }

	id   set_block(id id) override
	{
		_last_block = _current_block;
		_current_block = id;

		return _last_block;
	}
	void enter_block(id id) override
	{
		_current_block = id;
	}
	id   leave_block_and_kill() override
	{
		if (!is_in_block())
			return 0;

		code() += write_scope() + "discard;\n";

		return set_block(0);
	}
	id   leave_block_and_return(id value) override
	{
		if (!is_in_block())
			return 0;

		code() += write_scope() + "return" + (value ? ' ' + id_to_name(value) : std::string()) + ";\n";

		return set_block(0);
	}
	id   leave_block_and_switch(id, id) override
	{
		if (!is_in_block())
			return _last_block;

		return set_block(0);
	}
	id   leave_block_and_branch(id target, unsigned int loop_flow) override
	{
		if (!is_in_block())
			return _last_block;

		switch (loop_flow)
		{
		case 1:
			code() += write_scope() + "break;\n";
			break;
		case 2:
			code() += write_scope() + "continue;\n";
			break;
		case 3:
			_switch_fallthrough_blocks[_current_block].push_back(target);
			break;
		}

		return set_block(0);
	}
	id   leave_block_and_branch_conditional(id, id, id) override
	{
		if (!is_in_block())
			return _last_block;

		return set_block(0);
	}
	void leave_function() override
	{
		code() += "{\n" + _blocks[_last_block] + "}\n";

		assert(_scope_level > 0);
		_scope_level--;
	}
};

codegen *create_codegen_glsl()
{
	return new codegen_glsl();
}
