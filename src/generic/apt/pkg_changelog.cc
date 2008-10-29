// pkg_changelog.cc
//
//  Copyright 2000, 2004-2005, 2008 Daniel Burrows
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 2 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; see the file COPYING.  If not, write to
//  the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
//  Boston, MA 02111-1307, USA.

#include "pkg_changelog.h"

#include "apt.h"
#include "download_manager.h"
#include "pkg_acqfile.h"

#include <aptitude.h>
#include <config.h>

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>

#include <generic/util/util.h>

#include <apt-pkg/error.h>
#include <apt-pkg/configuration.h>
#include <apt-pkg/acquire-item.h>
#include <apt-pkg/strutl.h>

using namespace std;

class download_changelog_manager : public download_manager
{
public:
  // TODO: maybe I should have two callbacks, one for success and one
  // for failure?
  class entry
  {
    string srcpkg;
    string ver;
    string section;
    string name;
    sigc::slot1<void, temp::name> k;
    temp::name tempname;

  public:
    entry(const string &_srcpkg,
	  const string &_ver,
	  const string &_section,
	  const string &_name,
	  const temp::name &_tempname,
	  const sigc::slot1<void, temp::name> &_k)
      : srcpkg(_srcpkg),
	ver(_ver),
	section(_section),
	name(_name),
	k(_k),
	tempname(_tempname)
    {
    }

    const string &get_srcpkg() const { return srcpkg; }
    const string &get_ver() const { return ver; }
    const string &get_section() const { return section; }
    const string &get_name() const { return name; }
    const sigc::slot1<void, temp::name> &get_k() const { return k; }
    const temp::name &get_tempname() const { return tempname; }
  };

private:
  std::vector<entry> entries;

  download_signal_log *log;

  class AcqForEntry : public pkgAcqFile
  {
    entry ent;
    string real_uri;
  public:
    AcqForEntry(pkgAcquire *Owner,
		const string &URI,
		const string &MD5,
		unsigned long Size,
		const string &Description,
		const string &ShortDesc,
		const string &filename,
		const entry &_ent):
      pkgAcqFile(Owner, URI, MD5, Size, Description, ShortDesc, "", filename),
      ent(_ent),
      real_uri(URI)
    {
    }

    const entry &get_entry() const { return ent; }
    const string &get_real_uri() const { return real_uri; }
  };

public:
  template<typename Iter>
  download_changelog_manager(Iter begin, Iter end)
    : entries(begin, end), log(NULL)
  {
  }

  bool prepare(OpProgress &progress,
	       pkgAcquireStatus &acqlog,
	       download_signal_log *signallog)
  {
    log = signallog;

    fetcher = new pkgAcquire(&acqlog);

    for(std::vector<entry>::const_iterator it = entries.begin();
	it != entries.end(); ++it)
      {
	const string srcpkg(it->get_srcpkg());
	const string ver(it->get_ver());
	const string section(it->get_section());
	const string name(it->get_name());

	string realsection;

	if(section.find('/') != section.npos)
	  realsection.assign(section, 0, section.find('/'));
	else
	  realsection.assign("main");

	string prefix;

	if(srcpkg.size() > 3 &&
	   srcpkg[0] == 'l' && srcpkg[1] == 'i' && srcpkg[2] == 'b')
	  prefix = std::string("lib") + srcpkg[3];
	else
	  prefix = srcpkg[0];

	string realver;

	if(ver.find(':') != ver.npos)
	  realver.assign(ver, ver.find(':')+1, ver.npos);
	else
	  realver = ver;

	string uri = ssprintf("http://packages.debian.org/changelogs/pool/%s/%s/%s/%s_%s/changelog",
			      realsection.c_str(),
			      prefix.c_str(),
			      srcpkg.c_str(),
			      srcpkg.c_str(),
			      realver.c_str());

	string title = ssprintf(_("ChangeLog of %s"), name.c_str());

	new AcqForEntry(fetcher,
			uri,
			"",
			0,
			title,
			title,
			it->get_tempname().get_name().c_str(),
			*it);
      }

    return true;
  }

  void finish(pkgAcquire::RunResult res,
	      OpProgress *progress,
	      const sigc::slot1<void, result> &k)
  {
    if(fetcher != NULL)
      {
	result rval = success;

	for(pkgAcquire::ItemIterator it = fetcher->ItemsBegin();
	    it != fetcher->ItemsEnd(); ++it)
	  {
	    AcqForEntry *item(dynamic_cast<AcqForEntry *>(*it));
	    if(item != NULL)
	      {
		if(res != pkgAcquire::Continue || (*it)->Status != pkgAcquire::Item::StatDone)
		  {
		    _error->Error("Failed to fetch the description of %s from the URI %s: %s",
				  item->get_entry().get_srcpkg().c_str(),
				  item->get_real_uri().c_str(),
				  item->ErrorText.c_str());

		    rval = failure;
		  }
		else
		  {
		    const entry &ent(item->get_entry());

		    ent.get_k()(ent.get_tempname());
		  }
	      }
	  }

	k(rval);
	return;
      }
    else
      {
	k(failure);
	return;
      }
  }
};

download_manager *get_changelogs(const std::vector<std::pair<pkgCache::VerIterator, sigc::slot1<void, temp::name> > > &versions)
{
  std::vector<download_changelog_manager::entry> entries;

  for(std::vector<std::pair<pkgCache::VerIterator, sigc::slot1<void, temp::name> > >::const_iterator
	it = versions.begin(); it != versions.end(); ++it)
    {
      pkgCache::VerIterator ver(it->first);
      const sigc::slot1<void, temp::name> &k(it->second);

      if(ver.end())
	continue;

      if(ver.FileList().end())
	continue;

      // Look up the source package.
      pkgRecords::Parser &rec =
	apt_package_records->Lookup(ver.FileList());
      string srcpkg =
	rec.SourcePkg().empty() ? ver.ParentPkg().Name() : rec.SourcePkg();

      const string sourcever =
	rec.SourceVer().empty() ? ver.VerStr() : rec.SourceVer();

      temp::dir tempdir;
      temp::name tempname;

      try
	{
	  tempdir = temp::dir("aptitude");
	  tempname = temp::name(tempdir, "changelog");
	}
      catch(temp::TemporaryCreationFailure e)
	{
	  _error->Error("%s", e.errmsg().c_str());
	  continue;
	}

      entries.push_back(download_changelog_manager::entry(srcpkg, sourcever, ver.Section(), ver.ParentPkg().Name(), tempname, k));
    }

  return new download_changelog_manager(entries.begin(), entries.end());
}

download_manager *get_changelog_from_source(const string &srcpkg,
					    const string &ver,
					    const string &section,
					    const string &name,
					    const sigc::slot1<void, temp::name> &k)
{
  temp::dir tempdir;
  temp::name tempname;

  try
    {
      tempdir = temp::dir("aptitude");
      tempname = temp::name(tempdir, "changelog");
    }
  catch(temp::TemporaryCreationFailure e)
    {
      _error->Error("%s", e.errmsg().c_str());
      return false;
    }

  std::vector<download_changelog_manager::entry> entries;
  entries.push_back(download_changelog_manager::entry(srcpkg, ver, section, name, tempname, k));

  return new download_changelog_manager(entries.begin(), entries.end());
}
