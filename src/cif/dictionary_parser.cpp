/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2020 NKI/AVL, Netherlands Cancer Institute
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <cif++/cif/condition.hpp>
#include <cif++/cif/dictionary_parser.hpp>
#include <cif++/cif/file.hpp>
#include <cif++/cif/parser.hpp>

namespace cif
{

using namespace literals;

inline void replace_all(std::string &s, std::string_view pat, std::string_view rep)
{
	for (std::string::size_type i = s.find(pat); i != std::string::npos; i = s.find(pat, i))
		s.replace(i, pat.size(), rep.data(), rep.size());
}

class dictionary_parser : public parser
{
  public:
	dictionary_parser(validator &validator, std::istream &is, file &f)
		: parser(is, f)
		, m_validator(validator)
	{
	}

	void load_dictionary()
	{
		std::unique_ptr<datablock> dict;
		auto savedDatablock = m_datablock;

		try
		{
			while (m_lookahead != CIFToken::Eof)
			{
				switch (m_lookahead)
				{
					case CIFToken::GLOBAL:
						parse_global();
						break;

					default:
					{
						dict.reset(new datablock(m_token_value)); // dummy datablock, for constructing the validator only
						m_datablock = dict.get();

						match(CIFToken::DATA);
						parse_datablock();
						break;
					}
				}
			}
		}
		catch (const std::exception &ex)
		{
			error(ex.what());
		}

		// store all validators
		for (auto &ic : mCategoryValidators)
			m_validator.add_category_validator(std::move(ic));
		mCategoryValidators.clear();

		for (auto &iv : mItemValidators)
		{
			auto cv = m_validator.get_validator_for_category(iv.first);
			if (cv == nullptr)
				error("Undefined category '" + iv.first);

			for (auto &v : iv.second)
				const_cast<category_validator *>(cv)->addItemValidator(std::move(v));
		}

		// check all item validators for having a typeValidator

		if (dict)
			link_items();

		// store meta information
		datablock::iterator info;
		bool n;
		std::tie(info, n) = m_datablock->emplace("dictionary");
		if (n)
		{
			auto r = info->front();
			m_validator.set_name(r["title"].as<std::string>());
			m_validator.version(r["version"].as<std::string>());
		}

		m_datablock = savedDatablock;

		mItemValidators.clear();
	}

  private:
	void parse_save_frame() override
	{
		if (not m_collected_item_types)
			m_collected_item_types = collect_item_types();

		std::string saveFrameName = m_token_value;

		if (saveFrameName.empty())
			error("Invalid save frame, should contain more than just 'save_' here");

		bool isCategorySaveFrame = m_token_value[0] != '_';

		datablock dict(m_token_value);
		datablock::iterator cat = dict.end();

		match(CIFToken::SAVE);
		while (m_lookahead == CIFToken::LOOP or m_lookahead == CIFToken::Tag)
		{
			if (m_lookahead == CIFToken::LOOP)
			{
				cat = dict.end(); // should start a new category

				match(CIFToken::LOOP);

				std::vector<std::string> tags;
				while (m_lookahead == CIFToken::Tag)
				{
					std::string catName, item_name;
					std::tie(catName, item_name) = split_tag_name(m_token_value);

					if (cat == dict.end())
						std::tie(cat, std::ignore) = dict.emplace(catName);
					else if (not iequals(cat->name(), catName))
						error("inconsistent categories in loop_");

					tags.push_back(item_name);
					match(CIFToken::Tag);
				}

				while (m_lookahead == CIFToken::Value)
				{
					cat->emplace({});
					auto row = cat->back();

					for (auto tag : tags)
					{
						row[tag] = m_token_value;
						match(CIFToken::Value);
					}
				}

				cat = dict.end();
			}
			else
			{
				std::string catName, item_name;
				std::tie(catName, item_name) = split_tag_name(m_token_value);

				if (cat == dict.end() or not iequals(cat->name(), catName))
					std::tie(cat, std::ignore) = dict.emplace(catName);

				match(CIFToken::Tag);

				if (cat->empty())
					cat->emplace({});
				cat->back()[item_name] = m_token_value;

				match(CIFToken::Value);
			}
		}

		match(CIFToken::SAVE);

		if (isCategorySaveFrame)
		{
			std::string category;
			cif::tie(category) = dict["category"].front().get("id");

			std::vector<std::string> keys;
			for (auto k : dict["category_key"])
				keys.push_back(std::get<1>(split_tag_name(k["name"].as<std::string>())));

			iset groups;
			for (auto g : dict["category_group"])
				groups.insert(g["id"].as<std::string>());

			mCategoryValidators.push_back(category_validator{category, keys, groups});
		}
		else
		{
			// if the type code is missing, this must be a pointer, just skip it
			std::string typeCode;
			cif::tie(typeCode) = dict["item_type"].front().get("code");

			const type_validator *tv = nullptr;
			if (not(typeCode.empty() or typeCode == "?"))
				tv = m_validator.get_validator_for_type(typeCode);

			iset ess;
			for (auto e : dict["item_enumeration"])
				ess.insert(e["value"].as<std::string>());

			std::string defaultValue;
			cif::tie(defaultValue) = dict["item_default"].front().get("value");
			bool defaultIsNull = false;
			if (defaultValue.empty())
			{
				// TODO: Is this correct???
				for (auto r : dict["_item_default"])
				{
					defaultIsNull = r["value"].is_null();
					break;
				}
			}

			// collect the dict from our dataBlock and construct validators
			for (auto i : dict["item"])
			{
				std::string tagName, category, mandatory;

				cif::tie(tagName, category, mandatory) = i.get("name", "category_id", "mandatory_code");

				std::string catName, item_name;
				std::tie(catName, item_name) = split_tag_name(tagName);

				if (catName.empty() or item_name.empty())
					error("Invalid tag name in _item.name " + tagName);

				if (not iequals(category, catName) and not(category.empty() or category == "?"))
					error("specified category id does match the implicit category name for tag '" + tagName + '\'');
				else
					category = catName;

				auto &ivs = mItemValidators[category];

				auto vi = find(ivs.begin(), ivs.end(), item_validator{item_name});
				if (vi == ivs.end())
					ivs.push_back(item_validator{item_name, iequals(mandatory, "yes"), tv, ess, defaultValue, defaultIsNull});
				else
				{
					// need to update the itemValidator?
					if (vi->m_mandatory != (iequals(mandatory, "yes")))
					{
						if (VERBOSE > 2)
						{
							std::cerr << "inconsistent mandatory value for " << tagName << " in dictionary" << std::endl;

							if (iequals(tagName, saveFrameName))
								std::cerr << "choosing " << mandatory << std::endl;
							else
								std::cerr << "choosing " << (vi->m_mandatory ? "Y" : "N") << std::endl;
						}

						if (iequals(tagName, saveFrameName))
							vi->m_mandatory = (iequals(mandatory, "yes"));
					}

					if (vi->m_type != nullptr and tv != nullptr and vi->m_type != tv)
					{
						if (VERBOSE > 1)
							std::cerr << "inconsistent type for " << tagName << " in dictionary" << std::endl;
					}

					//				vi->mMandatory = (iequals(mandatory, "yes"));
					if (vi->m_type == nullptr)
						vi->m_type = tv;

					vi->m_enums.insert(ess.begin(), ess.end());

					// anything else yet?
					// ...
				}
			}

			// collect the dict from our dataBlock and construct validators
			for (auto i : dict["item_linked"])
			{
				std::string childTagName, parentTagName;

				cif::tie(childTagName, parentTagName) = i.get("child_name", "parent_name");

				mLinkedItems.emplace(childTagName, parentTagName);
			}
		}
	}

	void link_items()
	{
		if (not m_datablock)
			error("no datablock");

		auto &dict = *m_datablock;

		// links are identified by a parent category, a child category and a group ID

		using key_type = std::tuple<std::string, std::string, int>;

		std::map<key_type, size_t> linkIndex;

		// Each link group consists of a set of keys
		std::vector<std::tuple<std::vector<std::string>, std::vector<std::string>>> linkKeys;

		auto addLink = [&](size_t ix, const std::string &pk, const std::string &ck)
		{
			auto &&[pkeys, ckeys] = linkKeys.at(ix);

			bool found = false;
			for (size_t i = 0; i < pkeys.size(); ++i)
			{
				if (pkeys[i] == pk and ckeys[i] == ck)
				{
					found = true;
					break;
				}
			}

			if (not found)
			{
				pkeys.push_back(pk);
				ckeys.push_back(ck);
			}
		};

		auto &linkedGroupList = dict["pdbx_item_linked_group_list"];

		for (auto gl : linkedGroupList)
		{
			std::string child, parent;
			int link_group_id;
			cif::tie(child, parent, link_group_id) = gl.get("child_name", "parent_name", "link_group_id");

			auto civ = m_validator.get_validator_for_item(child);
			if (civ == nullptr)
				error("in pdbx_item_linked_group_list, item '" + child + "' is not specified");

			auto piv = m_validator.get_validator_for_item(parent);
			if (piv == nullptr)
				error("in pdbx_item_linked_group_list, item '" + parent + "' is not specified");

			key_type key{piv->m_category->m_name, civ->m_category->m_name, link_group_id};
			if (not linkIndex.count(key))
			{
				linkIndex[key] = linkKeys.size();
				linkKeys.push_back({});
			}

			size_t ix = linkIndex.at(key);
			addLink(ix, piv->m_tag, civ->m_tag);
		}

		// Only process inline linked items if the linked group list is absent
		if (linkedGroupList.empty())
		{
			// for links recorded in categories but not in pdbx_item_linked_group_list
			for (auto li : mLinkedItems)
			{
				std::string child, parent;
				std::tie(child, parent) = li;

				auto civ = m_validator.get_validator_for_item(child);
				if (civ == nullptr)
					error("in pdbx_item_linked_group_list, item '" + child + "' is not specified");

				auto piv = m_validator.get_validator_for_item(parent);
				if (piv == nullptr)
					error("in pdbx_item_linked_group_list, item '" + parent + "' is not specified");

				key_type key{piv->m_category->m_name, civ->m_category->m_name, 0};
				if (not linkIndex.count(key))
				{
					linkIndex[key] = linkKeys.size();
					linkKeys.push_back({});
				}

				size_t ix = linkIndex.at(key);
				addLink(ix, piv->m_tag, civ->m_tag);
			}
		}

		auto &linkedGroup = dict["pdbx_item_linked_group"];

		// now store the links in the validator
		for (auto &kv : linkIndex)
		{
			link_validator link = {};
			std::tie(link.m_parent_category, link.m_child_category, link.m_link_group_id) = kv.first;

			std::tie(link.m_parent_keys, link.m_child_keys) = linkKeys[kv.second];

			// look up the label
			for (auto r : linkedGroup.find("category_id"_key == link.m_child_category and "link_group_id"_key == link.m_link_group_id))
			{
				link.m_link_group_label = r["label"].as<std::string>();
				break;
			}

			m_validator.add_link_validator(std::move(link));
		}

		// now make sure the itemType is specified for all itemValidators

		for (auto &cv : m_validator.m_category_validators)
		{
			for (auto &iv : cv.m_item_validators)
			{
				if (iv.m_type == nullptr and cif::VERBOSE >= 0)
					std::cerr << "Missing item_type for " << iv.m_tag << std::endl;
			}
		}
	}

	bool collect_item_types()
	{
		bool result = false;

		if (not m_datablock)
			error("no datablock");

		auto &dict = *m_datablock;

		for (auto t : dict["item_type_list"])
		{
			std::string code, primitiveCode, construct;
			cif::tie(code, primitiveCode, construct) = t.get("code", "primitive_code", "construct");

			replace_all(construct, "\\n", "\n");
			replace_all(construct, "\\t", "\t");
			replace_all(construct, "\\\n", "");

			try
			{
				type_validator v = {
					code, map_to_primitive_type(primitiveCode), construct};

				m_validator.add_type_validator(std::move(v));
			}
			catch (const std::exception &)
			{
				std::throw_with_nested(parse_error(/*t.lineNr()*/ 0, "error in regular expression"));
			}

			// Do not replace an already defined type validator, this won't work with pdbx_v40
			// as it has a name that is too strict for its own names :-)
			//		if (mFileImpl.mTypeValidators.count(v))
			//			mFileImpl.mTypeValidators.erase(v);

			if (VERBOSE >= 5)
				std::cerr << "Added type " << code << " (" << primitiveCode << ") => " << construct << std::endl;

			result = true;
		}

		return result;
	}

	validator &m_validator;
	bool m_collected_item_types = false;

	std::vector<category_validator> mCategoryValidators;
	std::map<std::string, std::vector<item_validator>> mItemValidators;
	std::set<std::tuple<std::string, std::string>> mLinkedItems;
};

// --------------------------------------------------------------------

validator parse_dictionary(std::string_view name, std::istream &is)
{
	validator result(name);

	file f;
	dictionary_parser p(result, is, f);
	p.load_dictionary();

	return result;
}

} // namespace cif