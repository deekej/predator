/*
 * Copyright (C) 2011 Jiri Simacek
 *
 * This file is part of forester.
 *
 * forester is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * forester is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with forester.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef FOLDING_H
#define FOLDING_H

// Standard library headers
#include <vector>
#include <set>
#include <stdexcept>
#include <algorithm>
#include <unordered_map>

// Forester headers
#include "abstractbox.hh"
#include "boxman.hh"
#include "config.h"
#include "connection_graph.hh"
#include "forestautext.hh"
#include "restart_request.hh"
#include "streams.hh"

class Folding
{
private:  // data types

	typedef TreeAut::Transition Transition;

private:  // data members

	FAE& fae_;
	BoxMan& boxMan_;

	std::vector<std::pair<bool, ConnectionGraph::StateToCutpointSignatureMap>> signatureMap_;

protected:

	static void copyBox(
		std::vector<size_t>&                lhs,
		std::vector<const AbstractBox*>&    label,
		const AbstractBox*                  box,
		const std::vector<size_t>&          srcLhs,
		const size_t&                       srcOffset)
	{
		for (size_t i = 0; i < box->getArity(); ++i)
			lhs.push_back(srcLhs[srcOffset + i]);

		label.push_back(box);
	}

	static const ConnectionGraph::CutpointSignature& getSignature(
		size_t                                                 state,
		const ConnectionGraph::StateToCutpointSignatureMap&    signatures)
	{
		auto iter = signatures.find(state);

		assert(iter != signatures.end());

		return iter->second;
	}

	static bool isSignaturesCompatible(
		const ConnectionGraph::CutpointSignature&    s1,
		const ConnectionGraph::CutpointSignature&    s2);

	const ConnectionGraph::StateToCutpointSignatureMap& getSignatures(
		size_t        root);

	void invalidateSignatures(size_t root)
	{
		// Preconditions
		assert(root < signatureMap_.size());

		signatureMap_[root].first = false;
	}

	void componentCut(
		TreeAut&                                 res,
		TreeAut&                                 complement,
		ConnectionGraph::CutpointSignature&      complementSignature,
		size_t                                   root,
		size_t                                   state,
		size_t                                   target);

	std::pair<std::shared_ptr<TreeAut>, std::shared_ptr<TreeAut>> separateCutpoint(
		ConnectionGraph::CutpointSignature&            boxSignature,
		size_t                                         root,
		size_t                                         state,
		size_t                                         cutpoint);

	std::shared_ptr<TreeAut> relabelReferences(
		const                              TreeAut& ta,
		std::vector<size_t>&               index)
	{
		auto tmp = std::shared_ptr<TreeAut>(fae_.allocTA());

		fae_.relabelReferences(*tmp, ta, index);

		return tmp;
	}

	/**
	 * @brief  @todo
	 */
	std::shared_ptr<TreeAut> joinBox(
		const TreeAut&                               src,
		size_t                                       state,
		size_t                                       root,
		const Box*                                   box,
		const ConnectionGraph::CutpointSignature&    signature);

	static void updateSelectorMap(
		std::unordered_map<size_t, size_t>&          m,
		size_t                                       selector,
		const ConnectionGraph::CutpointSignature&    signature)
	{
		for (auto& cutpoint : signature)
		{
			auto p = m.insert(std::make_pair(cutpoint.root, selector));

			if (!p.second && p.first->second > selector)
				p.first->second = selector;
		}
	}

	// compute cutpoint-to-selector mapping, i.e. tell which selector one needs to take
	// in order to reach a given cutpoint
	static void computeSelectorMap(
		std::unordered_map<size_t, size_t>&                    selectorMap,
		const Transition&                                      t,
		const ConnectionGraph::StateToCutpointSignatureMap&    stateMap);

	bool checkSelectorMap(
		const std::unordered_map<size_t, size_t>&     selectorMap,
		size_t                                        root,
		size_t                                        state);

	bool computeSelectorMap(
		std::unordered_map<size_t, size_t>&      selectorMap,
		size_t                                   root,
		size_t                                   state);

	static size_t extractSelector(
		const std::unordered_map<size_t, size_t>&    selectorMap,
		size_t                                       target)
	{
		auto iter = selectorMap.find(target);

		assert(iter != selectorMap.end());

		return iter->second;
	}

	// transform
	static void extractInputMap(
		std::vector<size_t>&                         inputMap,
		const std::unordered_map<size_t, size_t>&    selectorMap,
		size_t                                       root,
		const std::vector<size_t>&                   index);

	const Box* getBox(const Box& box, bool conditional)
	{
		return (conditional)?(boxMan_.lookupBox(box)):(boxMan_.getBox(box));
	}

	const Box* makeType1Box(
		size_t                        root,
		size_t                        state,
		size_t                        aux,
		const std::set<size_t>&       forbidden,
		bool                          conditional = true,
		bool                          test = false);

	const Box* makeType2Box(
		size_t                      root,
		size_t                      aux,
		const std::set<size_t>&     forbidden,
		bool                        conditional = true,
		bool                        test = false);

public:

	bool discover1(
		size_t                       root,
		const std::set<size_t>&      forbidden,
		bool                         conditional);

	bool discover2(
		size_t                       root,
		const std::set<size_t>&      forbidden,
		bool                         conditional);

	bool discover3(
		size_t                      root,
		const std::set<size_t>&     forbidden,
		bool                        conditional);

public:

	Folding(
		FAE&           fae,
		BoxMan&        boxMan) :
		fae_(fae),
		boxMan_(boxMan),
		signatureMap_(fae.getRootCount())
	{ }
};

#endif
