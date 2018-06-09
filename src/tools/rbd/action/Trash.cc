// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2017 SUSE LINUX GmbH
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "tools/rbd/ArgumentTypes.h"
#include "tools/rbd/Shell.h"
#include "tools/rbd/Utils.h"
#include "common/errno.h"
#include "include/stringify.h"
#include "common/Formatter.h"
#include "common/TextTable.h"
#include "common/Clock.h"
#include <iostream>
#include <sstream>
#include <boost/program_options.hpp>
#include <boost/bind.hpp>
#include <json_spirit/json_spirit.h>

namespace rbd {
namespace action {
namespace trash {

namespace at = argument_types;
namespace po = boost::program_options;

//Optional arguments used only by this set of commands (rbd trash *)
static const std::string EXPIRES_AT("expires-at");
static const std::string EXPIRED_BEFORE("expired-before");
static const std::string THRESHOLD("threshold");

static bool is_not_trash_user(const librbd::trash_image_info_t &trash_info) {
  return trash_info.source != RBD_TRASH_IMAGE_SOURCE_USER;
}

void get_move_arguments(po::options_description *positional,
                        po::options_description *options) {
  at::add_image_spec_options(positional, options, at::ARGUMENT_MODIFIER_NONE);
  options->add_options()
    (EXPIRES_AT.c_str(), po::value<std::string>()->default_value("now"),
     "set the expiration time of an image so it can be purged when it is stale");
}

int execute_move(const po::variables_map &vm,
                 const std::vector<std::string> &ceph_global_init_args) {
  size_t arg_index = 0;
  std::string pool_name;
  std::string image_name;
  std::string snap_name;

  int r = utils::get_pool_image_snapshot_names(
    vm, at::ARGUMENT_MODIFIER_NONE, &arg_index, &pool_name, &image_name,
    &snap_name, utils::SNAPSHOT_PRESENCE_NONE, utils::SPEC_VALIDATION_NONE);
  if (r < 0) {
    return r;
  }

  librados::Rados rados;
  librados::IoCtx io_ctx;
  r = utils::init(pool_name, &rados, &io_ctx);
  if (r < 0) {
    return r;
  }

  utime_t now = ceph_clock_now();
  utime_t exp_time = now;
  std::string expires_at;
  if (vm.find(EXPIRES_AT) != vm.end()) {
    expires_at = vm[EXPIRES_AT].as<std::string>();
    r = utime_t::invoke_date(expires_at, &exp_time);
    if (r < 0) {
      std::cerr << "rbd: error calling /bin/date: " << cpp_strerror(r)
                << std::endl;
      return r;
    }
  }

  time_t dt = exp_time.sec() - now.sec();
  if(dt < 0) {
    std::cerr << "rbd: cannot use a date in the past as an expiration date"
              << std::endl;
    return -EINVAL;
  }

  librbd::RBD rbd;
  r = rbd.trash_move(io_ctx, image_name.c_str(), dt);
  if (r < 0) {
    std::cerr << "rbd: deferred delete error: " << cpp_strerror(r)
              << std::endl;
  }

  return r;
}

void get_remove_arguments(po::options_description *positional,
                          po::options_description *options) {
  positional->add_options()
    (at::IMAGE_ID.c_str(), "image id\n(example: [<pool-name>/]<image-id>)");
  at::add_pool_option(options, at::ARGUMENT_MODIFIER_NONE);
  at::add_image_id_option(options);

  at::add_no_progress_option(options);
  options->add_options()
      ("force", po::bool_switch(), "force remove of non-expired delayed images");
}

void remove_error_check(int r) {
    if (r == -ENOTEMPTY) {
      std::cerr << "rbd: image has snapshots - these must be deleted"
                << " with 'rbd snap purge' before the image can be removed."
                << std::endl;
    } else if (r == -EBUSY) {
      std::cerr << "rbd: error: image still has watchers"
                << std::endl
                << "This means the image is still open or the client using "
                << "it crashed. Try again after closing/unmapping it or "
                << "waiting 30s for the crashed client to timeout."
                << std::endl;
    } else if (r == -EMLINK) {
      std::cerr << std::endl
		<< "Remove the image from the group and try again."
		<< std::endl;
    } else if (r == -EPERM) {
      std::cerr << std::endl
                << "Deferment time has not expired, please use --force if you "
                << "really want to remove the image"
                << std::endl;
    } else {
      std::cerr << "rbd: remove error: " << cpp_strerror(r) << std::endl;
    }
}

int execute_remove(const po::variables_map &vm,
                   const std::vector<std::string> &ceph_global_init_args) {
  size_t arg_index = 0;
  std::string pool_name;
  std::string image_id;
  int r = utils::get_pool_image_id(vm, &arg_index, &pool_name, &image_id);
  if (r < 0) {
    return r;
  }

  librados::Rados rados;
  librados::IoCtx io_ctx;
  r = utils::init(pool_name, &rados, &io_ctx);
  if (r < 0) {
    return r;
  }

  io_ctx.set_osdmap_full_try();
  librbd::RBD rbd;

  utils::ProgressContext pc("Removing image", vm[at::NO_PROGRESS].as<bool>());
  r = rbd.trash_remove_with_progress(io_ctx, image_id.c_str(),
                                     vm["force"].as<bool>(), pc);
  if (r < 0) {
    remove_error_check(r); 
    pc.fail();
    return r;
  }

  pc.finish();

  return r;
}

std::string delete_status(time_t deferment_end_time) {
  time_t now = time(nullptr);

  std::string time_str = ctime(&deferment_end_time);
  time_str = time_str.substr(0, time_str.length() - 1);

  std::stringstream ss;
  if (now < deferment_end_time) {
    ss << "protected until " << time_str;
  } else {
    ss << "expired at " << time_str;
  }

  return ss.str();
}

int do_list(librbd::RBD &rbd, librados::IoCtx& io_ctx, bool long_flag,
            bool all_flag, Formatter *f) {
  std::vector<librbd::trash_image_info_t> trash_entries;
  int r = rbd.trash_list(io_ctx, trash_entries);
  if (r < 0) {
    return r;
  }

  if (!all_flag) {
    trash_entries.erase(remove_if(trash_entries.begin(),
                                  trash_entries.end(),
                                  boost::bind(is_not_trash_user, _1)),
                        trash_entries.end());
  }

  if (!long_flag) {
    if (f) {
      f->open_array_section("trash");
    }
    for (const auto& entry : trash_entries) {
       if (f) {
         f->open_object_section("image");
         f->dump_string("id", entry.id);
         f->dump_string("name", entry.name);
         f->close_section();
       } else {
         std::cout << entry.id << " " << entry.name << std::endl;
       }
    }
    if (f) {
      f->close_section();
      f->flush(std::cout);
    }
    return 0;
  }

  TextTable tbl;

  if (f) {
    f->open_array_section("trash");
  } else {
    tbl.define_column("ID", TextTable::LEFT, TextTable::LEFT);
    tbl.define_column("NAME", TextTable::LEFT, TextTable::LEFT);
    tbl.define_column("SOURCE", TextTable::LEFT, TextTable::LEFT);
    tbl.define_column("DELETED_AT", TextTable::LEFT, TextTable::LEFT);
    tbl.define_column("STATUS", TextTable::LEFT, TextTable::LEFT);
    tbl.define_column("PARENT", TextTable::LEFT, TextTable::LEFT);
  }

  for (const auto& entry : trash_entries) {
    librbd::Image im;

    r = rbd.open_by_id_read_only(io_ctx, im, entry.id.c_str(), NULL);
    // image might disappear between rbd.list() and rbd.open(); ignore
    // that, warn about other possible errors (EPERM, say, for opening
    // an old-format image, because you need execute permission for the
    // class method)
    if (r < 0) {
      if (r != -ENOENT) {
        std::cerr << "rbd: error opening " << entry.id << ": "
                  << cpp_strerror(r) << std::endl;
      }
      // in any event, continue to next image
      continue;
    }

    std::string del_source;
    switch (entry.source) {
      case RBD_TRASH_IMAGE_SOURCE_USER:
        del_source = "USER";
        break;
      case RBD_TRASH_IMAGE_SOURCE_MIRRORING:
        del_source = "MIRRORING";
        break;
    }

    std::string time_str = ctime(&entry.deletion_time);
    time_str = time_str.substr(0, time_str.length() - 1);

    bool has_parent = false;
    std::string pool, image, snap, parent;
    r = im.parent_info(&pool, &image, &snap);
    if (r == -ENOENT) {
      r = 0;
    } else if (r < 0) {
      return r;
    } else {
      parent = pool + "/" + image + "@" + snap;
      has_parent = true;
    }

    if (f) {
      f->open_object_section("image");
      f->dump_string("id", entry.id);
      f->dump_string("name", entry.name);
      f->dump_string("source", del_source);
      f->dump_string("deleted_at", time_str);
      f->dump_string("status",
                     delete_status(entry.deferment_end_time));
      if (has_parent) {
        f->open_object_section("parent");
        f->dump_string("pool", pool);
        f->dump_string("image", image);
        f->dump_string("snapshot", snap);
        f->close_section();
      }
      f->close_section();
    } else {
      tbl << entry.id
          << entry.name
          << del_source
          << time_str
          << delete_status(entry.deferment_end_time);
      if (has_parent)
        tbl << parent;
      tbl << TextTable::endrow;
    }
  }

  if (f) {
    f->close_section();
    f->flush(std::cout);
  } else if (!trash_entries.empty()) {
    std::cout << tbl;
  }

  return r < 0 ? r : 0;
}

void get_list_arguments(po::options_description *positional,
                        po::options_description *options) {
  at::add_pool_options(positional, options);
  options->add_options()
    ("all,a", po::bool_switch(), "list images from all sources");
  options->add_options()
    ("long,l", po::bool_switch(), "long listing format");
  at::add_format_options(options);
}

int execute_list(const po::variables_map &vm,
                 const std::vector<std::string> &ceph_global_init_args) {
  size_t arg_index = 0;
  std::string pool_name = utils::get_pool_name(vm, &arg_index);

  at::Format::Formatter formatter;
  int r = utils::get_formatter(vm, &formatter);
  if (r < 0) {
    return r;
  }

  librados::Rados rados;
  librados::IoCtx io_ctx;
  r = utils::init(pool_name, &rados, &io_ctx);
  if (r < 0) {
    return r;
  }

  utils::disable_cache();

  librbd::RBD rbd;
  r = do_list(rbd, io_ctx, vm["long"].as<bool>(), vm["all"].as<bool>(),
              formatter.get());
  if (r < 0) {
    std::cerr << "rbd: trash list: " << cpp_strerror(r) << std::endl;
    return r;
  }

  return 0;
}

void get_purge_arguments(po::options_description *positional,
                            po::options_description *options) {
  at::add_pool_options(positional, options);
  at::add_no_progress_option(options);
  
  options->add_options()
      (EXPIRED_BEFORE.c_str(), po::value<std::string>()->value_name("date"), 
       "purges images that expired before the given date");
  options->add_options()
      (THRESHOLD.c_str(), po::value<double>(), 
       "purges images until the current pool data usage is reduced to X%, "
       "value range: 0.0-1.0");
}


int execute_purge (const po::variables_map &vm,
                   const std::vector<std::string> &ceph_global_init_args) {
  size_t arg_index = 0;
  std::string pool_name = utils::get_pool_name(vm, &arg_index);

  librados::Rados rados;
  librados::IoCtx io_ctx;
  int r = utils::init(pool_name, &rados, &io_ctx);
  if (r < 0) {
    return r;
  }

  utils::disable_cache();

  io_ctx.set_osdmap_full_try();
  librbd::RBD rbd;
  
  std::vector<librbd::trash_image_info_t> trash_entries;
  r = rbd.trash_list(io_ctx, trash_entries);
  if (r < 0) { 
    return r;
  }

  std::remove_if(trash_entries.begin(), trash_entries.end(),
    [](librbd::trash_image_info_t info) {
      return info.source != RBD_TRASH_IMAGE_SOURCE_USER;
    }
  );

  std::vector<const char *> to_be_removed;

  if (vm.find(THRESHOLD) != vm.end()) {
    double threshold = vm[THRESHOLD].as<double>();
    if (threshold < 0 || threshold > 1) {
      std::cerr << "rbd: argument 'threshold' is out of valid range"
                << std::endl;
      return -EINVAL;
    }

    librados::bufferlist inbl;
    librados::bufferlist outbl;
    rados.mon_command("{\"prefix\": \"df\", \"format\": \"json\"}", inbl,
                      &outbl, NULL);
   

    json_spirit::mValue json;
    if(!json_spirit::read(outbl.to_str(), json)) {
      std::cerr << "rbd: ceph df json output could not be parsed"
                << std::endl;
      return -EBADMSG;
    }

    json_spirit::mArray arr = json.get_obj()["pools"].get_array();

    double pool_percent_used = 0;
    uint64_t pool_total_bytes = 0;

    std::map<std::string, std::vector<const char *>> datapools;

    std::sort(trash_entries.begin(), trash_entries.end(),
      [](librbd::trash_image_info_t a, librbd::trash_image_info_t b) {
        return a.deferment_end_time < b.deferment_end_time;
      }
    );

    for (const auto& entry : trash_entries) {
      librbd::Image image;
      std::string data_pool;
      r = utils::open_image_by_id(io_ctx, entry.id, true, &image);
      if(r < 0) continue;

      int64_t data_pool_id = image.get_data_pool_id();
      if (data_pool_id != io_ctx.get_id()) {
        librados::Rados rados(io_ctx);
        librados::IoCtx data_io_ctx;
        r = rados.ioctx_create2(data_pool_id, data_io_ctx);
        if (r < 0) {
          std::cerr << "rbd: error accessing data pool" << std::endl;
          continue;
        }
        data_pool = data_io_ctx.get_pool_name();
        datapools[data_pool].push_back(entry.id.c_str());
      } else {
        datapools[pool_name].push_back(entry.id.c_str());
      }
    }

    uint64_t bytes_to_free = 0;

    for(uint8_t i = 0; i < arr.size(); ++i) {
      json_spirit::mObject obj = arr[i].get_obj();
      std::string name = obj.find("name")->second.get_str();
      auto img = datapools.find(name);
      if(img != datapools.end()) {
        json_spirit::mObject stats =  arr[i].get_obj()["stats"].get_obj();
        pool_percent_used = stats["percent_used"].get_real();
        if(pool_percent_used <= threshold) continue;

        bytes_to_free = 0;

        pool_total_bytes = stats["max_avail"].get_uint64() +
                           stats["bytes_used"].get_uint64();
          
        auto bytes_threshold = (uint64_t)(pool_total_bytes * 
                               (pool_percent_used - threshold));
    
        librbd::Image curr_img;
        for(const auto &it : img->second){
          r = utils::open_image_by_id(io_ctx, it, true, &curr_img);
          if(r < 0) continue;
      
          uint64_t img_size; curr_img.size(&img_size);
          r = curr_img.diff_iterate2(nullptr, 0, img_size, false, true,
            [](uint64_t offset, size_t len, int exists, void *arg) {
              auto *to_free = reinterpret_cast<uint64_t*>(arg);
              if (exists) (*to_free) += len;
              return 0;
            }, &bytes_to_free
          );
          if(r < 0) continue;
          to_be_removed.push_back(it);
          if(bytes_to_free >= bytes_threshold) break;
        }
      }
    }
    if (bytes_to_free == 0) {
      std::cout << "rbd: pool usage is lower than or equal to "
                << (threshold*100)
                << "%" << endl;
      std::cout << "Nothing to do" << std::endl;
      return 0;
    }
  } else {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    time_t expire_ts = now.tv_sec;
    if (vm.find(EXPIRED_BEFORE) != vm.end()) { 
      utime_t new_time;
      r = utime_t::invoke_date(vm[EXPIRED_BEFORE].as<std::string>(), &new_time);
      if (r < 0) {
        std::cerr << "rbd: error calling /bin/date: " << cpp_strerror(r)
                  << std::endl;
        return r;
      }
      expire_ts = new_time.sec();
    }

    for(const auto &entry : trash_entries) {    
      if (expire_ts >= entry.deferment_end_time) {
        to_be_removed.push_back(entry.id.c_str());
      }
    }
  }

  uint64_t list_size = to_be_removed.size(), i = 0;

  if(list_size == 0) {
    std::cout << "rbd: nothing to remove" << std::endl;
  } else {
    utils::ProgressContext pc("Removing images", vm[at::NO_PROGRESS].as<bool>());
    for(const auto &entry_id : to_be_removed) {
      r = rbd.trash_remove(io_ctx, entry_id, true);
      if (r < 0) {
        remove_error_check(r);
        pc.fail();
        return r;
      }
      pc.update_progress(++i, list_size);
    }
    pc.finish();
  }

  return 0;
}

void get_restore_arguments(po::options_description *positional,
                            po::options_description *options) {
  positional->add_options()
    (at::IMAGE_ID.c_str(), "image id\n(example: [<pool-name>/]<image-id>)");
  at::add_pool_option(options, at::ARGUMENT_MODIFIER_NONE);
  at::add_image_id_option(options);
  at::add_image_option(options, at::ARGUMENT_MODIFIER_NONE, "");
}

int execute_restore(const po::variables_map &vm,
                    const std::vector<std::string> &ceph_global_init_args) {
  size_t arg_index = 0;
  std::string pool_name;
  std::string image_id;
  int r = utils::get_pool_image_id(vm, &arg_index, &pool_name, &image_id);
  if (r < 0) {
    return r;
  }

  librados::Rados rados;
  librados::IoCtx io_ctx;
  r = utils::init(pool_name, &rados, &io_ctx);
  if (r < 0) {
    return r;
  }

  std::string name;
  if (vm.find(at::IMAGE_NAME) != vm.end()) {
    name = vm[at::IMAGE_NAME].as<std::string>();
  }

  librbd::RBD rbd;
  r = rbd.trash_restore(io_ctx, image_id.c_str(), name.c_str());
  if (r < 0) {
    if (r == -ENOENT) {
      std::cerr << "rbd: error: image does not exist in trash"
                << std::endl;
    } else if (r == -EEXIST) {
      std::cerr << "rbd: error: an image with the same name already exists, "
                << "try again with with a different name"
                << std::endl;
    } else {
      std::cerr << "rbd: restore error: " << cpp_strerror(r) << std::endl;
    }
    return r;
  }

  return r;
}


Shell::Action action_move(
  {"trash", "move"}, {"trash", "mv"}, "Move an image to the trash.", "",
  &get_move_arguments, &execute_move);

Shell::Action action_remove(
  {"trash", "remove"}, {"trash", "rm"}, "Remove an image from trash.", "",
  &get_remove_arguments, &execute_remove);

Shell::Action action_purge(
  {"trash", "purge"}, {}, "Remove all expired images from trash.", "",
  &get_purge_arguments, &execute_purge);

Shell::SwitchArguments switched_arguments({"long", "l"});
Shell::Action action_list(
  {"trash", "list"}, {"trash", "ls"}, "List trash images.", "",
  &get_list_arguments, &execute_list);

Shell::Action action_restore(
  {"trash", "restore"}, {}, "Restore an image from trash.", "",
  &get_restore_arguments, &execute_restore);

} // namespace trash
} // namespace action
} // namespace rbd