// match.cc
//
//   Copyright (C) 2008 Daniel Burrows
//
//   This program is free software; you can redistribute it and/or
//   modify it under the terms of the GNU General Public License as
//   published by the Free Software Foundation; either version 2 of
//   the License, or (at your option) any later version.
//
//   This program is distributed in the hope that it will be useful,
//   but WITHOUT ANY WARRANTY; without even the implied warranty of
//   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//   General Public License for more details.
//
//   You should have received a copy of the GNU General Public License
//   along with this program; see the file COPYING.  If not, write to
//   the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
//   Boston, MA 02111-1307, USA.

#include "match.h"

#include <generic/apt/apt.h>

#include <apt-pkg/pkgrecords.h>
#include <apt-pkg/pkgsystem.h>
#include <apt-pkg/version.h>

#include <cwidget/generic/util/transcode.h>

#include <algorithm>

#include "serialize.h"

using cwidget::util::transcode;
using cwidget::util::ref_ptr;

namespace aptitude
{
  namespace matching
  {
    namespace
    {
      /** \brief Describes how version-by-version matching is carried
       *  out.
       */
      enum structural_eval_mode
	{
	  /** \brief All the versions in the current pool must match. */
	  structural_eval_all,

	  /** \brief Any one of the versions in the current pool can match. */
	  structural_eval_any
	};

      void print_matchable(std::ostream &out,
			   const matchable &matchable,
			   aptitudeDepCache &cache)
      {
	out << matchable.get_package_iterator(cache).Name();
	if(matchable.get_has_version())
	  {
	    out << " "
		<< matchable.get_version_iterator(cache).VerStr();
	  }
      }

      void print_pool(std::ostream &out,
		      const std::vector<matchable> &pool,
		      aptitudeDepCache &cache)
      {
	out << "{";
	for(std::vector<matchable>::const_iterator it =
	      pool.begin(); it != pool.end(); ++it)
	  {
	    if(it != pool.begin())
	      out << ", ";

	    print_matchable(out, *it, cache);
	  }
	out << "}";
      }

      // The evaluation stack holds references to pools (sorted lists
      // of matchables).
      //
      // NB: this is safe only because references to captured
      // variables can't escape (because you can't, e.g., get a handle
      // to a lambda and return it -- the only naming construct forces
      // the variables to be referred to in the dynamic scope of the
      // construct).  If this weren't the case, we'd need to
      // reference-count the values on the stack -- and if lambdas
      // could end up on the stack themselves, we'd have to fall back
      // to full garbage-collection (e.g., mark-and-sweep).
      typedef std::vector<const std::vector<matchable> *> stack;

      void print_stack(std::ostream &out,
		       const stack &stack,
		       aptitudeDepCache &cache)
      {
	out << "[";
	for(stack::const_reverse_iterator it =
	      stack.rbegin(); it != stack.rend(); ++it)
	  {
	    if(it != stack.rbegin())
	      out << " | ";

	    print_pool(out, **it, cache);
	  }
	out << "]";
      }

      ref_ptr<structural_match> evaluate_structural(structural_eval_mode mode,
						    const ref_ptr<pattern> &p,
						    stack &the_stack,
						    const std::vector<matchable> &pool,
						    aptitudeDepCache &cache,
						    pkgRecords &records,
						    bool debug);

      /** \brief Evaluate any regular expression-based pattern.
       *
       *  \param p      The pattern to evaluate.
       *  \param inf    The regular expression to apply.
       *  \param s      The string to test the regular expression against.
       *  \param invert \b true if this match is inverted (i.e., in a NOT
       *                context).  For inverted matches, we only return
       *                a match if the regex does \e not match, and the
       *                match region is the whole string.
       *
       *  \return     A match object corresponding to the regexp,
       *              or \b NULL if the match failed.
       */
      ref_ptr<match> evaluate_regexp(const ref_ptr<pattern> &p,
				     const pattern::regex_info &inf,
				     const char *s,
				     bool debug)
      {
	// Unfortunately, regexec() seems to require a hard limit to
	// the number of matches that can be returned. :-(
	regmatch_t matches[30];
	const int num_matches = sizeof(matches) / sizeof(regmatch_t);

	bool matched = inf.get_regex_group()->exec(s,
						   matches,
						   num_matches);

	if(matched)
	  {
	    int matches_found = 0;
	    while(matches_found < 30 && matches[matches_found].rm_so >= 0)
	      ++matches_found;

	    return match::make_regexp(p, matches, matches + matches_found);
	  }
	else
	  return NULL;
      }

      // Match an atomic expression against one matchable.
      ref_ptr<match> evaluate_atomic(const ref_ptr<pattern> &p,
				     const matchable &target,
				     stack &the_stack,
				     aptitudeDepCache &cache,
				     pkgRecords &records,
				     bool debug)
      {
	if(debug)
	  {
	    std::cout << "Matching " << serialize_pattern(p)
		      << " against the target ";
	    print_matchable(std::cout, target, cache);
	    std::cout << " with stack ";
	    print_stack(std::cout, the_stack, cache);
	    std::cout << std::endl;
	  }

	switch(p->get_type())
	  {
	    // Structural matchers:

	  case pattern::all_versions:
	  case pattern::and_tp:
	  case pattern::any_version:
	  case pattern::for_tp:
	  case pattern::narrow:
	  case pattern::not_tp:
	  case pattern::or_tp:
	  case pattern::widen:
	    throw MatchingException("Internal error: evaluate_atomic() invoked on a non-leaf node.");
	    break;

	    // Atomic matchers:
	  case pattern::archive:
	    if(!target.get_has_version())
	      return NULL;

	    {
	      pkgCache::VerIterator ver(target.get_version_iterator(cache));


	      for(pkgCache::VerFileIterator f = ver.FileList(); !f.end(); ++f)
		{
		  pkgCache::PkgFileIterator cur = f.File();

		  if(!cur.end() && cur.Archive())
		    {
		      ref_ptr<match> m = evaluate_regexp(p,
							 p->get_archive_regex_info(),
							 cur.Archive(),
							 debug);

		      if(m.valid())
			return m;
		    }
		}
	    }

	    return NULL;
	    break;

	  case pattern::action:
	    {
	      bool matches = false;
	      pattern::action_type type = p->get_action_action_type();
	      pkgCache::PkgIterator pkg = target.get_package_iterator(cache);

	      // Install, purge, and remove states all match more than
	      // one find_pkg_state return value.
	      switch(type)
		{
		case pattern::action_install:
		  {
		    pkg_action_state thetype = find_pkg_state(pkg, cache);
		    matches = thetype == pkg_install || thetype == pkg_auto_install;
		  }
		  break;

		case pattern::action_purge:
		  if((cache[pkg].iFlags & pkgDepCache::Purge) == 0)
		    matches = false;
		  else
		    {
		      pkg_action_state thetype = find_pkg_state(pkg, cache);
		      matches = thetype == pkg_remove || thetype == pkg_auto_remove || thetype == pkg_unused_remove;
		    }
		  break;

		case pattern::action_remove:
		  {
		    pkg_action_state thetype = find_pkg_state(pkg, cache);

		    matches = thetype == pkg_remove || thetype == pkg_auto_remove || thetype == pkg_unused_remove;
		  }
		  break;

		case pattern::action_hold:
		  matches = !pkg.CurrentVer().end() && cache.get_ext_state(pkg).selection_state == pkgCache::State::Hold;
		  break;

		  // The rest correspond directly to find_pkg_state() return values.
		case pattern::action_reinstall:
		  matches = find_pkg_state(pkg, cache) == pkg_reinstall;

		case pattern::action_upgrade:
		  matches = find_pkg_state(pkg, cache) == pkg_upgrade;

		case pattern::action_downgrade:
		  matches = find_pkg_state(pkg, cache) == pkg_downgrade;

		case pattern::action_keep:
		  matches = cache[pkg].Keep();
		  break;

		default:
		  throw MatchingException("Internal error: bad action-type flag.");
		}

	      if(matches)
		return match::make_atomic(p);
	      else
		return NULL;
	    }

	    break;

	  case pattern::automatic:
	    {
	      pkgCache::PkgIterator pkg(target.get_package_iterator(cache));

	      if(  (!pkg.CurrentVer().end() || cache[pkg].Install()) &&
		   (cache[pkg].Flags & pkgCache::Flag::Auto)  )
		return match::make_atomic(p);
	      else
		return NULL;
	    }
	    break;

	  case pattern::bind:
	    // If this assert fails, something went wrong internally.
	    {
	      const std::size_t variable_index = p->get_bind_variable_index();
	      eassert(variable_index >= 0 && variable_index < the_stack.size());

	      ref_ptr<structural_match>
		sub_match(evaluate_structural(structural_eval_any,
					      p->get_bind_pattern(),
					      the_stack,
					      *the_stack[variable_index],
					      cache,
					      records,
					      debug));

	      if(sub_match.valid())
		return match::make_with_sub_match(p, sub_match);
	      else
		return NULL;
	    }
	    break;

	  case pattern::broken:
	    if(!target.get_has_version())
	      return NULL;
	    else
	      {
		pkgCache::PkgIterator pkg(target.get_package_iterator(cache));
		aptitudeDepCache::StateCache &state = cache[pkg];

		if(state.NowBroken() || state.InstBroken())
		  return match::make_atomic(p);
		else
		  return NULL;
	      }
	    break;

	  case pattern::broken_type:
	    if(!target.get_has_version())
	      return NULL;
	    else
	      {
		pkgCache::VerIterator ver(target.get_version_iterator(cache));
		pkgCache::DepIterator dep(ver.DependsList());

		while(!dep.end())
		  {
		    while(dep->CompareOp & pkgCache::Dep::Or)
		      ++dep;

		    if(dep->Type == p->get_broken_type_depends_type() &&
		       !(cache[dep] & pkgDepCache::DepGInstall))
		      // Oops, it's broken..
		      return match::make_atomic(p);

		    ++dep;
		  }

		return NULL;
	      }
	    break;

	  case pattern::candidate_version:
	    if(!target.get_has_version())
	      return NULL;
	    else
	      {
		pkgCache::PkgIterator pkg(target.get_package_iterator(cache));
		pkgCache::VerIterator ver(target.get_version_iterator(cache));

		if(ver == cache[pkg].CandidateVerIter(cache))
		  return match::make_atomic(p);
		else
		  return NULL;
	      }
	    break;

	  case pattern::config_files:
	    if(target.get_pkg()->CurrentState == pkgCache::State::ConfigFiles)
	      return match::make_atomic(p);
	    else
	      return NULL;
	    break;

	  case pattern::current_version:
	    if(!target.get_has_version())
	      return NULL;
	    else
	      {
		pkgCache::PkgIterator pkg(target.get_package_iterator(cache));

		pkgCache::VerIterator ver(target.get_version_iterator(cache));

		if(ver == pkg.CurrentVer())
		  return match::make_atomic(p);
		else
		  return NULL;
	      }
	    break;

	  case pattern::depends:
	    if(!target.get_has_version())
	      return NULL;
	    else
	      {
		const pkgCache::VerIterator ver(target.get_version_iterator(cache));

		const pkgCache::Dep::DepType depends_type = p->get_depends_depends_type();
		const bool broken = p->get_depends_broken();

		pkgCache::DepIterator dep = ver.DependsList();
		while(!dep.end())
		  {
		    pkgCache::DepIterator or_group_start = dep;

		    if( (depends_type == dep->Type) ||
			(depends_type == pkgCache::Dep::Depends &&
			 dep->Type == pkgCache::Dep::PreDepends))
		      {
			if(broken)
			  {
			    pkgCache::DepIterator d2(cache, &*dep);
			    while(d2->CompareOp & pkgCache::Dep::Or)
			      ++d2;
			    if(cache[d2] & pkgDepCache::DepGInstall)
			      {
				dep = d2;
				++dep;
				continue;
			      }
			  }

			std::vector<matchable> new_pool;

			// See if a versionless match works.
			while(1)
			  {
			    pkgCache::PkgIterator pkg(dep.TargetPkg());
			    if(pkg.VersionList().end())
			      new_pool.push_back(matchable(pkg));
			    else
			      {
				for(pkgCache::VerIterator i=pkg.VersionList(); !i.end(); i++)
				  if(_system->VS->CheckDep(i.VerStr(), dep->CompareOp, dep.TargetVer()))
				    new_pool.push_back(matchable(pkg, i));
			      }

			    if((dep->CompareOp & pkgCache::Dep::Or) == 0)
			      break;
			    else
			      ++dep;
			  }

			if(!new_pool.empty())
			  {
			    std::sort(new_pool.begin(), new_pool.end());

			    ref_ptr<structural_match> m =
			      evaluate_structural(structural_eval_any,
						  p->get_depends_pattern(),
						  the_stack,
						  new_pool,
						  cache,
						  records,
						  debug);

			    // Note: the dependency that we return is
			    // just the head of the OR group.
			    if(m.valid())
			      return match::make_dependency(p, m,
							    or_group_start);
			  }
		      }

		    ++dep;
		  }

		return NULL;
	      }
	    break;

	  case pattern::description:
	    if(!target.get_has_version())
	      return NULL;
	    else
	      {
		pkgCache::VerIterator ver(target.get_version_iterator(cache));
		return evaluate_regexp(p,
				       p->get_description_regex_info(),
				       transcode(get_long_description(ver, &records)).c_str(),
				       debug);
	      }
	    break;

	  case pattern::essential:
	    {
	      pkgCache::PkgIterator pkg(target.get_package_iterator(cache));

	      if(  ((pkg->Flags & pkgCache::Flag::Essential) == pkgCache::Flag::Essential) ||
		   ((pkg->Flags & pkgCache::Flag::Important) == pkgCache::Flag::Important)  )
		  return match::make_atomic(p);
	      else
		return NULL;
	    }
	    break;

	  case pattern::equal:
	    {
	      const std::size_t variable_index = p->get_equal_stack_position();
	      eassert(variable_index >= 0 && variable_index < the_stack.size());

	      // Search for the incoming package/version in the pool
	      // referenced by this pattern.
	      const std::vector<matchable> &pool(*the_stack[variable_index]);

	      if(std::binary_search(pool.begin(), pool.end(),
				    target))
		return match::make_atomic(p);
	      else
		return NULL;
	    }

	    break;

	  case pattern::false_tp:
	    return NULL;
	    break;

	  case pattern::garbage:
	    if(!target.get_has_version())
	      return NULL;
	    else if(!cache[target.get_package_iterator(cache)].Garbage)
	      return NULL;
	    else
	      return match::make_atomic(p);
	    break;

	  case pattern::install_version:
	    return NULL;
	    break;

	  case pattern::installed:
	    return NULL;
	    break;

	  case pattern::maintainer:
	    return NULL;
	    break;

	  case pattern::name:
	    return evaluate_regexp(p,
				   p->get_name_regex_info(),
				   target.get_package_iterator(cache).Name(),
				   debug);
	    break;

	  case pattern::new_tp:
	    return NULL;
	    break;

	  case pattern::obsolete:
	    return NULL;
	    break;

	  case pattern::origin:
	    return NULL;
	    break;

	  case pattern::priority:
	    return NULL;
	    break;

	  case pattern::provides:
	    return NULL;
	    break;

	  case pattern::reverse_depends:
	    return NULL;
	    break;

	  case pattern::reverse_provides:
	    return NULL;
	    break;

	  case pattern::section:
	    return NULL;
	    break;

	  case pattern::source_package:
	    return NULL;
	    break;

	  case pattern::source_version:
	    return NULL;
	    break;

	  case pattern::tag:
	    return NULL;
	    break;

	  case pattern::task:
	    return NULL;
	    break;

	  case pattern::true_tp:
	    return NULL;
	    break;

	  case pattern::upgradable:
	    return NULL;
	    break;

	  case pattern::user_tag:
	    return NULL;
	    break;

	  case pattern::version:
	    if(!target.get_has_version())
	      return NULL;

	    return evaluate_regexp(p,
				   p->get_version_regex_info(),
				   target.get_version_iterator(cache).VerStr(),
				   debug);
	    break;

	  case pattern::virtual_tp:
	    return NULL;
	    break;

	  default:
	    throw MatchingException("Internal error: unhandled pattern type in evaluate()");
	  }
      }

      ref_ptr<structural_match> evaluate_structural(structural_eval_mode mode,
						    const ref_ptr<pattern> &p,
						    stack &the_stack,
						    const std::vector<matchable> &pool,
						    aptitudeDepCache &cache,
						    pkgRecords &records,
						    bool debug)
      {
	if(debug)
	  {
	    std::cout << "Matching " << serialize_pattern(p)
		      << " against the pool ";
	    print_pool(std::cout, pool, cache);
	    std::cout << " with stack ";
	    print_stack(std::cout, the_stack, cache);
	    std::cout << " (mode=";
	    switch(mode)
	      {
	      case structural_eval_all:
		std::cout << "all";
		break;

	      case structural_eval_any:
		std::cout << "any";
		break;
	      }
	    std::cout << ")" << std::endl;
	  }

	switch(p->get_type())
	  {
	    // Structural matchers:

	  case pattern::all_versions:
	    {
	      ref_ptr<structural_match>
		m(evaluate_structural(structural_eval_all,
				      p->get_all_versions_pattern(),
				      the_stack,
				      pool,
				      cache,
				      records,
				      debug));

	      if(!m.valid())
		return NULL;
	      else
		return structural_match::make_branch(p, &m, (&m) + 1);
	    }
	    break;

	  case pattern::and_tp:
	    {
	      const std::vector<ref_ptr<pattern> > &sub_patterns(p->get_and_patterns());
	      std::vector<ref_ptr<structural_match> > sub_matches;

	      for(std::vector<ref_ptr<pattern> >::const_iterator it =
		    sub_patterns.begin(); it != sub_patterns.end(); ++it)
		{
		  ref_ptr<structural_match> m(evaluate_structural(mode,
								  (*it),
								  the_stack,
								  pool,
								  cache,
								  records,
								  debug));

		  if(!m.valid())
		    return NULL;

		  sub_matches.push_back(m);
		}

	      return structural_match::make_branch(p, sub_matches.begin(), sub_matches.end());
	    }
	    break;

	  case pattern::any_version:
	    {
	      std::vector<matchable> new_pool;
	      new_pool.push_back(matchable());

	      std::vector<ref_ptr<structural_match> > sub_matches;
	      for(std::vector<matchable>::const_iterator it =
		    pool.begin(); it != pool.end(); ++it)
		{
		  new_pool[0] = *it;

		  ref_ptr<structural_match>
		    m(evaluate_structural(mode,
					  p->get_any_version_pattern(),
					  the_stack,
					  new_pool,
					  cache,
					  records,
					  debug));

		  if(m.valid())
		    sub_matches.push_back(m);
		}

	      if(sub_matches.empty())
		return NULL;
	      else
		return structural_match::make_branch(p, sub_matches.begin(), sub_matches.end());
	    }
	    break;

	  case pattern::for_tp:
	    {
	      the_stack.push_back(&pool);

	      const ref_ptr<structural_match>
		m(evaluate_structural(mode,
				      p->get_for_pattern(),
				      the_stack,
				      pool,
				      cache,
				      records,
				      debug));

	      if(m.valid())
		return structural_match::make_branch(p, &m, (&m) + 1);
	      else
		return NULL;
	    }
	    break;

	  case pattern::narrow:
	    // Match each entry in the pool against the filter
	    // separately.  Then match the main pattern against a
	    // pool formed from values that passed the filter.
	    {
	      std::vector<matchable> singleton_pool;
	      std::vector<matchable> new_pool;
	      singleton_pool.push_back(matchable());

	      // \todo we should perhaps store the filter matches in a
	      // separate list.
	      for(std::vector<matchable>::const_iterator it =
		    pool.begin(); it != pool.end(); ++it)
		{
		  singleton_pool[0] = *it;

		  if(evaluate_structural(mode,
					 p->get_narrow_filter(),
					 the_stack,
					 singleton_pool,
					 cache,
					 records,
					 debug).valid())
		    new_pool.push_back(*it);
		}

	      if(new_pool.empty())
		return NULL;
	      else
		{
		  ref_ptr<structural_match>
		    m(evaluate_structural(mode,
					  p->get_narrow_pattern(),
					  the_stack,
					  new_pool,
					  cache,
					  records,
					  debug));

		  if(!m.valid())
		    return NULL;
		  else
		    return structural_match::make_branch(p, &m, (&m) + 1);
		}
	    }
	    break;

	  case pattern::not_tp:
	    {
	      ref_ptr<structural_match> m(evaluate_structural(mode,
							      p->get_not_pattern(),
							      the_stack,
							      pool,
							      cache,
							      records,
							      debug));

	      if(!m.valid())
		// Report a structural match with no sub-parts.  This
		// will lose doubly-negated information.  For now that's
		// just too bad; we can try to recover it later.
		return structural_match::make_branch(p,
						     (ref_ptr<structural_match> *)0,
						     (ref_ptr<structural_match> *)0);
	      else
		return NULL;
	    }

	    break;

	  case pattern::or_tp:
	    {
	      const std::vector<ref_ptr<pattern> > &sub_patterns(p->get_or_patterns());
	      std::vector<ref_ptr<structural_match> > sub_matches;

	      // Note: we do *not* short-circuit, in order to allow
	      // the caller to see as much information as possible
	      // about the match.
	      for(std::vector<ref_ptr<pattern> >::const_iterator it =
		    sub_patterns.begin(); it != sub_patterns.end(); ++it)
		{
		  ref_ptr<structural_match> m(evaluate_structural(mode,
								  (*it),
								  the_stack,
								  pool,
								  cache,
								  records,
								  debug));

		  if(m.valid())
		    sub_matches.push_back(m);
		}

	      if(sub_matches.empty())
		return NULL;
	      else
		return structural_match::make_branch(p, sub_matches.begin(), sub_matches.end());
	    }
	    break;

	  case pattern::widen:
	    // NB: to make this fast I rely on the sort order of
	    // matchables.
	    //
	    // \todo Perhaps this pattern should be redefined to allow
	    // us to inject arbitrary stuff into the pool?  Right now
	    // it just expands the pool to include all the versions of
	    // each package that it includes.
	    {
	      std::vector<matchable> new_pool;

	      for(std::vector<matchable>::const_iterator it
		    = pool.begin(); it != pool.end(); ++it)
		{
		  // If we've already seen this package it'll be at
		  // the back of the new pool (due to processing
		  // inputs in the pool in sort order).
		  if(!new_pool.empty() &&
		     new_pool.back().get_pkg() == it->get_pkg())
		    continue;

		  // Virtual packages aren't touched by ?widen.
		  if(!it->get_has_version())
		    {
		      new_pool.push_back(*it);
		      continue;
		    }

		  pkgCache::PkgIterator pkg =
		    it->get_package_iterator(cache);
		  if(pkg.VersionList().end())
		    new_pool.push_back(*it);
		  else
		    {
		      for(pkgCache::VerIterator ver = pkg.VersionList();
			  !ver.end(); ++ver)
			{
			  new_pool.push_back(matchable(pkg, ver));
			}
		    }
		}

	      std::sort(new_pool.begin(), new_pool.end());
	      ref_ptr<structural_match>
		m(evaluate_structural(mode,
				      p->get_widen_pattern(),
				      the_stack,
				      new_pool,
				      cache,
				      records,
				      debug));
	      if(!m.valid())
		return NULL;
	      else
		return structural_match::make_branch(p, &m, (&m) + 1);
	    }
	    break;

	    // Atomic matchers:

	  case pattern::archive:
	  case pattern::action:
	  case pattern::automatic:
	  case pattern::bind:
	  case pattern::broken:
	  case pattern::broken_type:
	  case pattern::candidate_version:
	  case pattern::config_files:
	  case pattern::current_version:
	  case pattern::depends:
	  case pattern::description:
	  case pattern::essential:
	  case pattern::equal:
	  case pattern::false_tp:
	  case pattern::garbage:
	  case pattern::install_version:
	  case pattern::installed:
	  case pattern::maintainer:
	  case pattern::name:
	  case pattern::new_tp:
	  case pattern::obsolete:
	  case pattern::origin:
	  case pattern::priority:
	  case pattern::provides:
	  case pattern::reverse_depends:
	  case pattern::reverse_provides:
	  case pattern::section:
	  case pattern::source_package:
	  case pattern::source_version:
	  case pattern::tag:
	  case pattern::task:
	  case pattern::true_tp:
	  case pattern::upgradable:
	  case pattern::user_tag:
	  case pattern::version:
	  case pattern::virtual_tp:
	    switch(mode)
	      {
	      case structural_eval_all:
		{
		  std::vector<std::pair<matchable, ref_ptr<match> > > matches;
		  for(std::vector<matchable>::const_iterator it =
			pool.begin(); it != pool.end(); ++it)
		    {
		      cwidget::util::ref_ptr<match> m(evaluate_atomic(p, *it, the_stack, cache, records, debug));
		      if(!m.valid())
			{
			  if(debug)
			    {
			      std::cout << "Failed to match: ";
			      print_matchable(std::cout, *it, cache);
			      std::cout << std::endl;
			    }
			  return NULL;
			}
		      else
			matches.push_back(std::make_pair(*it, m));
		    }

		  if(matches.size() == 0)
		    return NULL;
		  else
		    return structural_match::make_leaf(p, matches.begin(), matches.end());
		}
		break;

	      case structural_eval_any:
		{
		  std::vector<std::pair<matchable, ref_ptr<match> > > matches;
		  for(std::vector<matchable>::const_iterator it =
			pool.begin(); it != pool.end(); ++it)
		    {
		      cwidget::util::ref_ptr<match> m(evaluate_atomic(p, *it, the_stack, cache, records, debug));
		      if(m.valid())
			{
			  if(debug)
			    {
			      std::cout << "Matched: ";
			      print_matchable(std::cout, *it, cache);
			      std::cout << std::endl;
			    }
			  // TODO: short-circuit?
			  matches.push_back(std::make_pair(*it, m));
			}
		    }

		  if(matches.size() == 0)
		    return NULL;
		  else
		    return structural_match::make_leaf(p, matches.begin(), matches.end());
		}
		break;

	      default:
		throw MatchingException("Internal error: unhandled structural match mode.");
	      }

	    break;

	  default:
	    throw MatchingException("Internal error: unhandled pattern type in evaluate()");
	  }
      }
    }

    ref_ptr<structural_match>
    get_match(const ref_ptr<pattern> &p,
	      const pkgCache::PkgIterator &pkg,
	      const pkgCache::VerIterator &ver,
	      aptitudeDepCache &cache,
	      pkgRecords &records,
	      bool debug)
    {
      std::vector<matchable> initial_pool;

      if(pkg.VersionList().end())
	initial_pool.push_back(matchable(pkg));
      else if(ver.end())
	{
	  for(pkgCache::VerIterator ver2 = pkg.VersionList();
	      !ver2.end(); ++ver2)
	    {
	      initial_pool.push_back(matchable(pkg, ver2));
	    }
	}
      else
	{
	  eassert(ver.ParentPkg() == pkg);

	  initial_pool.push_back(matchable(pkg, ver));
	}

      std::sort(initial_pool.begin(), initial_pool.end());

      stack st;
      st.push_back(&initial_pool);

      return evaluate_structural(structural_eval_any,
				 p,
				 st,
				 initial_pool,
				 cache,
				 records,
				 debug);
    }

    ref_ptr<structural_match>
    get_match(const ref_ptr<pattern> &p,
	      const pkgCache::PkgIterator &pkg,
	      aptitudeDepCache &cache,
	      pkgRecords &records,
	      bool debug)
    {
      return get_match(p, pkg,
		       pkgCache::VerIterator(cache),
		       cache, records, debug);
    }
  }
}

