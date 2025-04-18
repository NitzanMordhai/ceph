// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph distributed storage system
 *
 * Copyright (C) 2014 Cloudwatt <libre.licensing@cloudwatt.com>
 * Copyright (C) 2014 Red Hat <contact@redhat.com>
 *
 * Author: Loic Dachary <loic@dachary.org>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 */

#include <errno.h>
#include <memory>
#include <stdlib.h>

#include "crush/CrushWrapper.h"
#include "include/stringify.h"
#include "erasure-code/lrc/ErasureCodeLrc.h"
#include "global/global_context.h"
#include "common/config_proxy.h"
#include "gtest/gtest.h"

using namespace std;

TEST(ErasureCodeLrc, parse_rule)
{
  ErasureCodeLrc lrc(g_conf().get_val<std::string>("erasure_code_dir"));
  EXPECT_EQ("default", lrc.rule_root);
  EXPECT_EQ("host", lrc.rule_steps.front().type);

  ErasureCodeProfile profile;
  profile["crush-root"] = "other";
  EXPECT_EQ(0, lrc.parse_rule(profile, &cerr));
  EXPECT_EQ("other", lrc.rule_root);

  profile["crush-steps"] = "[]";
  EXPECT_EQ(0, lrc.parse_rule(profile, &cerr));
  EXPECT_TRUE(lrc.rule_steps.empty());

  profile["crush-steps"] = "0";
  EXPECT_EQ(ERROR_LRC_ARRAY, lrc.parse_rule(profile, &cerr));

  profile["crush-steps"] = "{";
  EXPECT_EQ(ERROR_LRC_PARSE_JSON, lrc.parse_rule(profile, &cerr));

  profile["crush-steps"] = "[0]";
  EXPECT_EQ(ERROR_LRC_ARRAY, lrc.parse_rule(profile, &cerr));

  profile["crush-steps"] = "[[0]]";
  EXPECT_EQ(ERROR_LRC_RULE_OP, lrc.parse_rule(profile, &cerr));

  profile["crush-steps"] = "[[\"choose\", 0]]";
  EXPECT_EQ(ERROR_LRC_RULE_TYPE, lrc.parse_rule(profile, &cerr));

  profile["crush-steps"] = "[[\"choose\", \"host\", []]]";
  EXPECT_EQ(ERROR_LRC_RULE_N, lrc.parse_rule(profile, &cerr));

  profile["crush-steps"] = "[[\"choose\", \"host\", 2]]";
  EXPECT_EQ(0, lrc.parse_rule(profile, &cerr));

  const ErasureCodeLrc::Step &step = lrc.rule_steps.front();
  EXPECT_EQ("choose", step.op);
  EXPECT_EQ("host", step.type);
  EXPECT_EQ(2, step.n);

  profile["crush-steps"] =
    "["
    " [\"choose\", \"rack\", 2], "
    " [\"chooseleaf\", \"host\", 5], "
    "]";
  EXPECT_EQ(0, lrc.parse_rule(profile, &cerr));
  EXPECT_EQ(2U, lrc.rule_steps.size());
  {
    const ErasureCodeLrc::Step &step = lrc.rule_steps[0];
    EXPECT_EQ("choose", step.op);
    EXPECT_EQ("rack", step.type);
    EXPECT_EQ(2, step.n);
  }
  {
    const ErasureCodeLrc::Step &step = lrc.rule_steps[1];
    EXPECT_EQ("chooseleaf", step.op);
    EXPECT_EQ("host", step.type);
    EXPECT_EQ(5, step.n);
  }
}

TEST(ErasureCodeTest, create_rule)
{
  auto c = std::make_unique<CrushWrapper>();
  c->create();
  int root_type = 3;
  c->set_type_name(root_type, "root");
  int rack_type = 2;
  c->set_type_name(rack_type, "rack");
  int host_type = 1;
  c->set_type_name(host_type, "host");
  int osd_type = 0;
  c->set_type_name(osd_type, "osd");

  int rootno;
  c->add_bucket(0, CRUSH_BUCKET_STRAW, CRUSH_HASH_RJENKINS1,
		root_type, 0, NULL, NULL, &rootno);
  c->set_item_name(rootno, "default");

  map<string,string> loc;
  loc["root"] = "default";

  //
  // Set all to 10 so that the item number it trivial to decompose
  // into rack/host/osd.
  //
  int num_rack;
  int num_host;
  int num_osd;
  num_rack = num_host = num_osd = 10;
  int osd = 0;
  for (int r=0; r<num_rack; ++r) {
    loc["rack"] = string("rack-") + stringify(r);
    for (int h=0; h<num_host; ++h) {
      loc["host"] = string("host-") + stringify(r) + string("-") + stringify(h);
      for (int o=0; o<num_osd; ++o, ++osd) {
	c->insert_item(g_ceph_context, osd, 1.0, string("osd.") + stringify(osd), loc);
      }
    }
  }

  c->finalize();

  ErasureCodeLrc lrc(g_conf().get_val<std::string>("erasure_code_dir"));
  EXPECT_EQ(0, lrc.create_rule("rule1", *c, &cerr));

  ErasureCodeProfile profile;
  unsigned int racks = 2;
  unsigned int hosts = 5;
  profile["crush-steps"] =
    "["
    " [\"choose\", \"rack\", " + stringify(racks) + "], "
    " [\"chooseleaf\", \"host\", " + stringify(hosts) + "], "
    "]";
  const char *rule_name = "rule2";
  EXPECT_EQ(0, lrc.parse_rule(profile, &cerr));
  EXPECT_EQ(1, lrc.create_rule(rule_name, *c, &cerr));

  vector<__u32> weight;
  for (int o = 0; o < c->get_max_devices(); o++)
    weight.push_back(0x10000);
  int rule = c->get_rule_id(rule_name);
  vector<int> out;
  unsigned int n = racks * hosts;
  c->do_rule(rule, 1, out, n, weight, 0);
  EXPECT_EQ(n, out.size());
  //
  // check that the first five are in the same rack and the next five
  // in the same rack
  //
  int first_rack = out[0] / num_host / num_osd;
  EXPECT_EQ(first_rack, out[1] / num_host / num_osd);
  EXPECT_EQ(first_rack, out[2] / num_host / num_osd);
  EXPECT_EQ(first_rack, out[3] / num_host / num_osd);
  EXPECT_EQ(first_rack, out[4] / num_host / num_osd);
  int second_rack = out[5] / num_host / num_osd;
  EXPECT_EQ(second_rack, out[6] / num_host / num_osd);
  EXPECT_EQ(second_rack, out[7] / num_host / num_osd);
  EXPECT_EQ(second_rack, out[8] / num_host / num_osd);
  EXPECT_EQ(second_rack, out[9] / num_host / num_osd);
}

TEST(ErasureCodeLrc, parse_kml)
{
  ErasureCodeLrc lrc(g_conf().get_val<std::string>("erasure_code_dir"));
  ErasureCodeProfile profile;
  EXPECT_EQ(0, lrc.parse_kml(profile, &cerr));
  profile["k"] = "4";
  EXPECT_EQ(ERROR_LRC_ALL_OR_NOTHING, lrc.parse_kml(profile, &cerr));
  const char *generated[] = { "mapping",
			      "layers",
			      "crush-steps" };
  profile["m"] = "2";
  profile["l"] = "3";

  for (int i = 0; i < 3; i++) {
    profile[generated[i]] = "SET";
    EXPECT_EQ(ERROR_LRC_GENERATED, lrc.parse_kml(profile, &cerr));
    profile.erase(profile.find(generated[i]));
  }

  profile["k"] = "4";
  profile["m"] = "2";
  profile["l"] = "7";
  EXPECT_EQ(ERROR_LRC_K_M_MODULO, lrc.parse_kml(profile, &cerr));

  profile["k"] = "3";
  profile["m"] = "3";
  profile["l"] = "3";
  EXPECT_EQ(ERROR_LRC_K_MODULO, lrc.parse_kml(profile, &cerr));

  profile["k"] = "4";
  profile["m"] = "2";
  profile["l"] = "3";
  EXPECT_EQ(0, lrc.parse_kml(profile, &cerr));
  EXPECT_EQ("[ "
	    " [ \"DDc_DDc_\", \"\" ],"
	    " [ \"DDDc____\", \"\" ],"
	    " [ \"____DDDc\", \"\" ],"
	    "]", profile["layers"]);
  EXPECT_EQ("DD__DD__", profile["mapping"]);
  EXPECT_EQ("chooseleaf", lrc.rule_steps[0].op);
  EXPECT_EQ("host", lrc.rule_steps[0].type);
  EXPECT_EQ(0, lrc.rule_steps[0].n);
  EXPECT_EQ(1U, lrc.rule_steps.size());
  profile.erase(profile.find("mapping"));
  profile.erase(profile.find("layers"));

  profile["k"] = "4";
  profile["m"] = "2";
  profile["l"] = "3";
  profile["crush-failure-domain"] = "osd";
  EXPECT_EQ(0, lrc.parse_kml(profile, &cerr));
  EXPECT_EQ("chooseleaf", lrc.rule_steps[0].op);
  EXPECT_EQ("osd", lrc.rule_steps[0].type);
  EXPECT_EQ(0, lrc.rule_steps[0].n);
  EXPECT_EQ(1U, lrc.rule_steps.size());
  profile.erase(profile.find("mapping"));
  profile.erase(profile.find("layers"));

  profile["k"] = "4";
  profile["m"] = "2";
  profile["l"] = "3";
  profile["crush-failure-domain"] = "osd";
  profile["crush-locality"] = "rack";
  EXPECT_EQ(0, lrc.parse_kml(profile, &cerr));
  EXPECT_EQ("choose", lrc.rule_steps[0].op);
  EXPECT_EQ("rack", lrc.rule_steps[0].type);
  EXPECT_EQ(2, lrc.rule_steps[0].n);
  EXPECT_EQ("chooseleaf", lrc.rule_steps[1].op);
  EXPECT_EQ("osd", lrc.rule_steps[1].type);
  EXPECT_EQ(4, lrc.rule_steps[1].n);
  EXPECT_EQ(2U, lrc.rule_steps.size());
  profile.erase(profile.find("mapping"));
  profile.erase(profile.find("layers"));
}

TEST(ErasureCodeLrc, layers_description)
{
  ErasureCodeLrc lrc(g_conf().get_val<std::string>("erasure_code_dir"));
  ErasureCodeProfile profile;

  json_spirit::mArray description;
  EXPECT_EQ(ERROR_LRC_DESCRIPTION,
	    lrc.layers_description(profile, &description, &cerr));

  {
    const char *description_string = "\"not an array\"";
    profile["layers"] = description_string;
    EXPECT_EQ(ERROR_LRC_ARRAY,
	      lrc.layers_description(profile, &description, &cerr));
  }
  {
    const char *description_string = "invalid json";
    profile["layers"] = description_string;
    EXPECT_EQ(ERROR_LRC_PARSE_JSON,
	      lrc.layers_description(profile, &description, &cerr));
  }
  {
    const char *description_string = "[]";
    profile["layers"] = description_string;
    EXPECT_EQ(0, lrc.layers_description(profile, &description, &cerr));
  }
}

TEST(ErasureCodeLrc, layers_parse)
{
  {
    ErasureCodeLrc lrc(g_conf().get_val<std::string>("erasure_code_dir"));
    ErasureCodeProfile profile;

    const char *description_string ="[ 0 ]";
    profile["layers"] = description_string;
    json_spirit::mArray description;
    EXPECT_EQ(0, lrc.layers_description(profile, &description, &cerr));
    EXPECT_EQ(ERROR_LRC_ARRAY,
	      lrc.layers_parse(description_string, description, &cerr));
  }

  {
    ErasureCodeLrc lrc(g_conf().get_val<std::string>("erasure_code_dir"));
    ErasureCodeProfile profile;

    const char *description_string ="[ [ 0 ] ]";
    profile["layers"] = description_string;
    json_spirit::mArray description;
    EXPECT_EQ(0, lrc.layers_description(profile, &description, &cerr));
    EXPECT_EQ(ERROR_LRC_STR,
	      lrc.layers_parse(description_string, description, &cerr));
  }

  {
    ErasureCodeLrc lrc(g_conf().get_val<std::string>("erasure_code_dir"));
    ErasureCodeProfile profile;

    const char *description_string ="[ [ \"\", 0 ] ]";
    profile["layers"] = description_string;
    json_spirit::mArray description;
    EXPECT_EQ(0, lrc.layers_description(profile, &description, &cerr));
    EXPECT_EQ(ERROR_LRC_CONFIG_OPTIONS,
	      lrc.layers_parse(description_string, description, &cerr));
  }

  //
  // The second element can be an object describing the plugin
  // profile.
  //
  {
    ErasureCodeLrc lrc(g_conf().get_val<std::string>("erasure_code_dir"));
    ErasureCodeProfile profile;

    const char *description_string ="[ [ \"\", { \"a\": \"b\" }, \"ignored\" ] ]";
    profile["layers"] = description_string;
    json_spirit::mArray description;
    EXPECT_EQ(0, lrc.layers_description(profile, &description, &cerr));
    EXPECT_EQ(0, lrc.layers_parse(description_string, description, &cerr));
    EXPECT_EQ("b", lrc.layers.front().profile["a"]);
  }

  //
  // The second element can be a str_map parseable string describing the plugin
  // profile.
  //
  {
    ErasureCodeLrc lrc(g_conf().get_val<std::string>("erasure_code_dir"));
    ErasureCodeProfile profile;

    const char *description_string ="[ [ \"\", \"a=b c=d\" ] ]";
    profile["layers"] = description_string;
    json_spirit::mArray description;
    EXPECT_EQ(0, lrc.layers_description(profile, &description, &cerr));
    EXPECT_EQ(0, lrc.layers_parse(description_string, description, &cerr));
    EXPECT_EQ("b", lrc.layers.front().profile["a"]);
    EXPECT_EQ("d", lrc.layers.front().profile["c"]);
  }

}

TEST(ErasureCodeLrc, layers_sanity_checks)
{
  {
    ErasureCodeLrc lrc(g_conf().get_val<std::string>("erasure_code_dir"));
    ErasureCodeProfile profile;
    profile["mapping"] =
	    "__DDD__DD";
    const char *description_string =
      "[ "
      "  [ \"_cDDD_cDD\", \"\" ],"
      "  [ \"c_DDD____\", \"\" ],"
      "  [ \"_____cDDD\", \"\" ],"
      "]";
    profile["layers"] = description_string;
    EXPECT_EQ(0, lrc.init(profile, &cerr));
  }
  {
    ErasureCodeLrc lrc(g_conf().get_val<std::string>("erasure_code_dir"));
    ErasureCodeProfile profile;
    const char *description_string =
      "[ "
      "]";
    profile["layers"] = description_string;
    EXPECT_EQ(ERROR_LRC_MAPPING, lrc.init(profile, &cerr));
  }
  {
    ErasureCodeLrc lrc(g_conf().get_val<std::string>("erasure_code_dir"));
    ErasureCodeProfile profile;
    profile["mapping"] = "";
    const char *description_string =
      "[ "
      "]";
    profile["layers"] = description_string;
    EXPECT_EQ(ERROR_LRC_LAYERS_COUNT, lrc.init(profile, &cerr));
  }
  {
    ErasureCodeLrc lrc(g_conf().get_val<std::string>("erasure_code_dir"));
    ErasureCodeProfile profile;
    profile["mapping"] =
	    "DD";
    const char *description_string =
      "[ "
      "  [ \"DD??\", \"\" ], "
      "  [ \"DD\", \"\" ], "
      "  [ \"DD\", \"\" ], "
      "]";
    profile["layers"] = description_string;
    EXPECT_EQ(-EINVAL, lrc.init(profile, &cerr));
  }
}

TEST(ErasureCodeLrc, layers_init)
{
  {
    ErasureCodeLrc lrc(g_conf().get_val<std::string>("erasure_code_dir"));
    ErasureCodeProfile profile;

    const char* env = getenv("CEPH_LIB");
    string directory(env ? env : "lib");
    string description_string = 
      "[ " 
      "  [ \"_cDDD_cDD_\", \"directory=" + directory + "\" ]," 
      "]";
    profile["layers"] = description_string;
    json_spirit::mArray description;
    EXPECT_EQ(0, lrc.layers_description(profile, &description, &cerr));
    EXPECT_EQ(0, lrc.layers_parse(description_string, description, &cerr));
    EXPECT_EQ(0, lrc.layers_init(&cerr));
    EXPECT_EQ("5", lrc.layers.front().profile["k"]);
    EXPECT_EQ("2", lrc.layers.front().profile["m"]);
    EXPECT_EQ("isa", lrc.layers.front().profile["plugin"]);
    EXPECT_EQ("reed_sol_van", lrc.layers.front().profile["technique"]);
  }
}

TEST(ErasureCodeLrc, init)
{
  ErasureCodeLrc lrc(g_conf().get_val<std::string>("erasure_code_dir"));
  ErasureCodeProfile profile;
  profile["mapping"] =
    "__DDD__DD";
  const char *description_string =
    "[ "
    "  [ \"_cDDD_cDD\", \"\" ],"
    "  [ \"c_DDD____\", \"\" ],"
    "  [ \"_____cDDD\", \"\" ],"
    "]";
  profile["layers"] = description_string;
  EXPECT_EQ(0, lrc.init(profile, &cerr));
}

TEST(ErasureCodeLrc, init_kml)
{
  ErasureCodeLrc lrc(g_conf().get_val<std::string>("erasure_code_dir"));
  ErasureCodeProfile profile;
  profile["k"] = "4";
  profile["m"] = "2";
  profile["l"] = "3";
  EXPECT_EQ(0, lrc.init(profile, &cerr));
  EXPECT_EQ((unsigned int)(4 + 2 + (4 + 2) / 3), lrc.get_chunk_count());
}

TEST(ErasureCodeLrc, minimum_to_decode)
{
  // trivial : no erasures, the minimum is want_to_read
  {
    ErasureCodeLrc lrc(g_conf().get_val<std::string>("erasure_code_dir"));
    ErasureCodeProfile profile;
    profile["mapping"] =
      "__DDD__DD";
    const char *description_string =
      "[ "
      "  [ \"_cDDD_cDD\", \"\" ],"
      "  [ \"c_DDD____\", \"\" ],"
      "  [ \"_____cDDD\", \"\" ],"
      "]";
    profile["layers"] = description_string;
    EXPECT_EQ(0, lrc.init(profile, &cerr));
    shard_id_set want_to_read;
    want_to_read.insert(shard_id_t(1));
    shard_id_set available_chunks;
    available_chunks.insert(shard_id_t(1));
    available_chunks.insert(shard_id_t(2));
    shard_id_set minimum;
    EXPECT_EQ(0, lrc._minimum_to_decode(want_to_read, available_chunks, &minimum));
    EXPECT_EQ(want_to_read, minimum);
  }
  // locally repairable erasure
  {
    ErasureCodeLrc lrc(g_conf().get_val<std::string>("erasure_code_dir"));
    ErasureCodeProfile profile;
    profile["mapping"] =
	    "__DDD__DD_";
    const char *description_string =
      "[ "
      "  [ \"_cDDD_cDD_\", \"\" ],"
      "  [ \"c_DDD_____\", \"\" ],"
      "  [ \"_____cDDD_\", \"\" ],"
      "  [ \"_____DDDDc\", \"\" ],"
      "]";
    profile["layers"] = description_string;
    EXPECT_EQ(0, lrc.init(profile, &cerr));
    EXPECT_EQ(profile["mapping"].length(),
	      lrc.get_chunk_count());
    {
      // want to read the last chunk
      shard_id_set want_to_read;
      want_to_read.insert(shard_id_t(lrc.get_chunk_count() - 1));
      // all chunks are available except the last chunk
      shard_id_set available_chunks;
      for (int i = 0; i < (int)lrc.get_chunk_count() - 1; i++)
	available_chunks.insert(shard_id_t(i));
      // _____DDDDc can recover c
      shard_id_set minimum;
      EXPECT_EQ(0, lrc._minimum_to_decode(want_to_read, available_chunks, &minimum));
      shard_id_set expected_minimum;
      expected_minimum.insert(shard_id_t(5));
      expected_minimum.insert(shard_id_t(6));
      expected_minimum.insert(shard_id_t(7));
      expected_minimum.insert(shard_id_t(8));
      EXPECT_EQ(expected_minimum, minimum);
    }
    {
      shard_id_set want_to_read;
      want_to_read.insert(shard_id_t(0));
      shard_id_set available_chunks;
      for (int i = 1; i < (int)lrc.get_chunk_count(); i++)
	available_chunks.insert(shard_id_t(i));
      shard_id_set minimum;
      EXPECT_EQ(0, lrc._minimum_to_decode(want_to_read, available_chunks, &minimum));
      shard_id_set expected_minimum;
      expected_minimum.insert(shard_id_t(2));
      expected_minimum.insert(shard_id_t(3));
      expected_minimum.insert(shard_id_t(4));
      EXPECT_EQ(expected_minimum, minimum);
    }
  }
  // implicit parity required
  {
    ErasureCodeLrc lrc(g_conf().get_val<std::string>("erasure_code_dir"));
    ErasureCodeProfile profile;
    profile["mapping"] =
	    "__DDD__DD";
    const char *description_string =
      "[ "
      "  [ \"_cDDD_cDD\", \"\" ],"
      "  [ \"c_DDD____\", \"\" ],"
      "  [ \"_____cDDD\", \"\" ],"
      "]";
    profile["layers"] = description_string;
    EXPECT_EQ(0, lrc.init(profile, &cerr));
    EXPECT_EQ(profile["mapping"].length(),
	      lrc.get_chunk_count());
    shard_id_set want_to_read;
    want_to_read.insert(shard_id_t(8));
    //
    // unable to recover, too many chunks missing
    //
    {
      shard_id_set available_chunks;
      available_chunks.insert(shard_id_t(0));
      available_chunks.insert(shard_id_t(1));
      // missing             (2)
      // missing             (3)
      available_chunks.insert(shard_id_t(4));
      available_chunks.insert(shard_id_t(5));
      available_chunks.insert(shard_id_t(6));
      // missing             (7)
      // missing             (8)
      shard_id_set minimum;
      EXPECT_EQ(-EIO, lrc._minimum_to_decode(want_to_read, available_chunks, &minimum));
    }
    //
    // We want to read chunk 8 and encoding was done with
    //
    //     _cDDD_cDD
    //	   c_DDD____
    //	   _____cDDD
    //
    // First strategy fails:
    //
    // 012345678
    // xxXXXxxXX  initial chunks
    // xx.XXxx..  missing (2, 7, 8)
    // _____cDDD  fail : can recover 1 but 2 are missing
    // c_DDD____  ignored because 8 is not used (i.e. _)
    // _cDDD_cDD  fail : can recover 2 but 3 are missing
    //
    // Second strategy succeeds:
    //
    // 012345678
    // xxXXXxxXX  initial chunks
    // xx.XXxx..  missing (2, 7, 8)
    // _____cDDD  fail : can recover 1 but 2 are missing
    // c_DDD____  success: recovers chunk 2
    // _cDDD_cDD  success: recovers chunk 7, 8
    //
    {
      shard_id_set available_chunks;
      available_chunks.insert(shard_id_t(0));
      available_chunks.insert(shard_id_t(1));
      // missing             (2)
      available_chunks.insert(shard_id_t(3));
      available_chunks.insert(shard_id_t(4));
      available_chunks.insert(shard_id_t(5));
      available_chunks.insert(shard_id_t(6));
      // missing             (7)
      // missing             (8)
      shard_id_set minimum;
      EXPECT_EQ(0, lrc._minimum_to_decode(want_to_read, available_chunks, &minimum));
      EXPECT_EQ(available_chunks, minimum);
    }
  }
}

TEST(ErasureCodeLrc, encode_decode)
{
  ErasureCodeLrc lrc(g_conf().get_val<std::string>("erasure_code_dir"));
  ErasureCodeProfile profile;
  profile["mapping"] =
    "__DD__DD";
  const char *description_string =
    "[ "
    "  [ \"_cDD_cDD\", \"\" ]," // global layer
    "  [ \"c_DD____\", \"\" ]," // first local layer
    "  [ \"____cDDD\", \"\" ]," // second local layer
    "]";
  profile["layers"] = description_string;
  EXPECT_EQ(0, lrc.init(profile, &cerr));
  EXPECT_EQ(4U, lrc.get_data_chunk_count());
  unsigned int chunk_size = g_conf().get_val<Option::size_t>("osd_pool_erasure_code_stripe_unit");
  unsigned int stripe_width = lrc.get_data_chunk_count() * chunk_size;
  EXPECT_EQ(chunk_size, lrc.get_chunk_size(stripe_width));
  shard_id_set want_to_encode;
  shard_id_map<bufferlist> encoded(lrc.get_chunk_count());
  for (unsigned int i = 0; i < lrc.get_chunk_count(); ++i) {
    want_to_encode.insert(shard_id_t(i));
    bufferptr ptr(buffer::create_page_aligned(chunk_size));
    bufferlist tmp;
    tmp.push_back(ptr);
    tmp.claim_append(encoded[shard_id_t(i)]);
    encoded[shard_id_t(i)].swap(tmp);
  }
  const vector<shard_id_t> &mapping = lrc.get_chunk_mapping();
  char c = 'A';
  for (unsigned int i = 0; i < lrc.get_data_chunk_count(); i++) {
    shard_id_t j = mapping[i];
    string s(chunk_size, c);
    encoded[j].clear();
    encoded[j].append(s);
    c++;
  }
  shard_id_map<bufferptr> in(lrc.get_chunk_count());
  shard_id_map<bufferptr> out(lrc.get_chunk_count());
  for (auto&& [shard, list] : encoded) {
    auto bp = list.begin().get_current_ptr();
    if (shard < lrc.get_data_chunk_count()) in[shard] = bp;
    else out[shard] = bp;
  }
  EXPECT_EQ(0, lrc.encode_chunks(in, out));

  {
    shard_id_map<bufferlist> chunks(lrc.get_chunk_count());
    chunks[shard_id_t(4)] = encoded[shard_id_t(4)];
    chunks[shard_id_t(5)] = encoded[shard_id_t(5)];
    chunks[shard_id_t(6)] = encoded[shard_id_t(6)];
    shard_id_set want_to_read;
    want_to_read.insert(shard_id_t(7));
    shard_id_set available_chunks;
    available_chunks.insert(shard_id_t(4));
    available_chunks.insert(shard_id_t(5));
    available_chunks.insert(shard_id_t(6));
    shard_id_set minimum;
    EXPECT_EQ(0, lrc._minimum_to_decode(want_to_read, available_chunks, &minimum));
    // only need three chunks from the second local layer
    EXPECT_EQ(3U, minimum.size());
    EXPECT_EQ(1U, minimum.count(shard_id_t(4)));
    EXPECT_EQ(1U, minimum.count(shard_id_t(5)));
    EXPECT_EQ(1U, minimum.count(shard_id_t(6)));
    shard_id_map<bufferlist> decoded(lrc.get_chunk_count());
    EXPECT_EQ(0, lrc._decode(want_to_read, chunks, &decoded));
    string s(chunk_size, 'D');
    EXPECT_EQ(s, string(decoded[shard_id_t(7)].c_str(), chunk_size));
  }
  {
    shard_id_set want_to_read;
    want_to_read.insert(shard_id_t(2));
    shard_id_map<bufferlist> chunks(lrc.get_chunk_count());
    chunks[shard_id_t(1)] = encoded[shard_id_t(1)];
    chunks[shard_id_t(3)] = encoded[shard_id_t(3)];
    chunks[shard_id_t(5)] = encoded[shard_id_t(5)];
    chunks[shard_id_t(6)] = encoded[shard_id_t(6)];
    chunks[shard_id_t(7)] = encoded[shard_id_t(7)];
    shard_id_set available_chunks;
    available_chunks.insert(shard_id_t(1));
    available_chunks.insert(shard_id_t(3));
    available_chunks.insert(shard_id_t(5));
    available_chunks.insert(shard_id_t(6));
    available_chunks.insert(shard_id_t(7));
    shard_id_set minimum;
    EXPECT_EQ(0, lrc._minimum_to_decode(want_to_read, available_chunks, &minimum));
    EXPECT_EQ(5U, minimum.size());
    EXPECT_EQ(available_chunks, minimum);

    shard_id_map<bufferlist> decoded(lrc.get_chunk_count());
    EXPECT_EQ(0, lrc._decode(want_to_read, encoded, &decoded));
    string s(chunk_size, 'A');
    EXPECT_EQ(s, string(decoded[shard_id_t(2)].c_str(), chunk_size));
  }
  {
    shard_id_set want_to_read;
    want_to_read.insert(shard_id_t(3));
    want_to_read.insert(shard_id_t(6));
    want_to_read.insert(shard_id_t(7));
    shard_id_set available_chunks;
    available_chunks.insert(shard_id_t(0));
    available_chunks.insert(shard_id_t(1));
    available_chunks.insert(shard_id_t(2));
    // available_chunks.insert(shard_id_t(3));
    available_chunks.insert(shard_id_t(4));
    available_chunks.insert(shard_id_t(5));
    // available_chunks.insert(shard_id_t(6));
    // available_chunks.insert(shard_id_t(7));
    encoded.erase(shard_id_t(3));
    encoded.erase(shard_id_t(6));
    shard_id_set minimum;
    EXPECT_EQ(0, lrc._minimum_to_decode(want_to_read, available_chunks, &minimum));
    EXPECT_EQ(4U, minimum.size());
    // only need two chunks from the first local layer
    EXPECT_EQ(1U, minimum.count(shard_id_t(0)));
    EXPECT_EQ(1U, minimum.count(shard_id_t(2)));
    // the above chunks will rebuild chunk 3 and the global layer only needs
    // three more chunks to reach the required amount of chunks (4) to recover
    // the last two
    EXPECT_EQ(1U, minimum.count(shard_id_t(1)));
    EXPECT_EQ(1U, minimum.count(shard_id_t(2)));
    EXPECT_EQ(1U, minimum.count(shard_id_t(5)));

    shard_id_map<bufferlist> decoded(lrc.get_chunk_count());
    EXPECT_EQ(0, lrc._decode(want_to_read, encoded, &decoded));
    {
      string s(chunk_size, 'B');
      EXPECT_EQ(s, string(decoded[shard_id_t(3)].c_str(), chunk_size));
    }
    {
      string s(chunk_size, 'C');
      EXPECT_EQ(s, string(decoded[shard_id_t(6)].c_str(), chunk_size));
    }
    {
      string s(chunk_size, 'D');
      EXPECT_EQ(s, string(decoded[shard_id_t(7)].c_str(), chunk_size));
    }
  }
}

TEST(ErasureCodeLrc, encode_decode_2)
{
  ErasureCodeLrc lrc(g_conf().get_val<std::string>("erasure_code_dir"));
  ErasureCodeProfile profile;
  profile["mapping"] =
    "DD__DD__";
  const char *description_string =
    "[ "
    " [ \"DDc_DDc_\", \"\" ],"
    " [ \"DDDc____\", \"\" ],"
    " [ \"____DDDc\", \"\" ],"
    "]";
  profile["layers"] = description_string;
  EXPECT_EQ(0, lrc.init(profile, &cerr));
  EXPECT_EQ(4U, lrc.get_data_chunk_count());
  unsigned int chunk_size = g_conf().get_val<Option::size_t>("osd_pool_erasure_code_stripe_unit");
  unsigned int stripe_width = lrc.get_data_chunk_count() * chunk_size;
  EXPECT_EQ(chunk_size, lrc.get_chunk_size(stripe_width));
  shard_id_set want_to_encode;
  shard_id_map<bufferlist> encoded(lrc.get_chunk_count());
  for (unsigned int i = 0; i < lrc.get_chunk_count(); ++i) {
    want_to_encode.insert(shard_id_t(i));
    bufferptr ptr(buffer::create_page_aligned(chunk_size));
    bufferlist tmp;
    tmp.push_back(ptr);
    tmp.claim_append(encoded[shard_id_t(i)]);
    encoded[shard_id_t(i)].swap(tmp);
  }
  const vector<shard_id_t> &mapping = lrc.get_chunk_mapping();
  char c = 'A';
  for (unsigned int i = 0; i < lrc.get_data_chunk_count(); i++) {
    shard_id_t j = mapping[i];
    string s(chunk_size, c);
    encoded[j].clear();
    encoded[j].append(s);
    c++;
  }
  shard_id_map<bufferptr> in(lrc.get_chunk_count());
  shard_id_map<bufferptr> out(lrc.get_chunk_count());
  for (auto&& [shard, list] : encoded) {
    auto bp = list.begin().get_current_ptr();
    if (shard < lrc.get_data_chunk_count()) in[shard] = bp;
    else out[shard] = bp;
  }
  EXPECT_EQ(0, lrc.encode_chunks(in, out));

  {
    shard_id_set want_to_read;
    want_to_read.insert(shard_id_t(0));
    shard_id_map<bufferlist> chunks(lrc.get_chunk_count());
    chunks[shard_id_t(1)] = encoded[shard_id_t(1)];
    chunks[shard_id_t(3)] = encoded[shard_id_t(3)];
    chunks[shard_id_t(4)] = encoded[shard_id_t(4)];
    chunks[shard_id_t(5)] = encoded[shard_id_t(5)];
    chunks[shard_id_t(6)] = encoded[shard_id_t(6)];
    chunks[shard_id_t(7)] = encoded[shard_id_t(7)];
    shard_id_set available_chunks;
    available_chunks.insert(shard_id_t(1));
    available_chunks.insert(shard_id_t(3));
    available_chunks.insert(shard_id_t(4));
    available_chunks.insert(shard_id_t(5));
    available_chunks.insert(shard_id_t(6));
    available_chunks.insert(shard_id_t(7));
    shard_id_set minimum;
    EXPECT_EQ(0, lrc._minimum_to_decode(want_to_read, available_chunks, &minimum));
    EXPECT_EQ(4U, minimum.size());
    EXPECT_EQ(1U, minimum.count(shard_id_t(1)));
    EXPECT_EQ(1U, minimum.count(shard_id_t(4)));
    EXPECT_EQ(1U, minimum.count(shard_id_t(5)));
    EXPECT_EQ(1U, minimum.count(shard_id_t(6)));

    shard_id_map<bufferlist> decoded(lrc.get_chunk_count());
    EXPECT_EQ(0, lrc._decode(want_to_read, chunks, &decoded));
    string s(chunk_size, 'A');
    EXPECT_EQ(s, string(decoded[shard_id_t(0)].c_str(), chunk_size));
  }
  {
    shard_id_set want_to_read;
    for (unsigned int i = 0; i < lrc.get_chunk_count(); i++)
      want_to_read.insert(shard_id_t(i));
    shard_id_map<bufferlist> chunks(lrc.get_chunk_count());
    chunks[shard_id_t(1)] = encoded[shard_id_t(1)];
    chunks[shard_id_t(3)] = encoded[shard_id_t(3)];
    chunks[shard_id_t(5)] = encoded[shard_id_t(5)];
    chunks[shard_id_t(6)] = encoded[shard_id_t(6)];
    chunks[shard_id_t(7)] = encoded[shard_id_t(7)];
    shard_id_set available_chunks;
    available_chunks.insert(shard_id_t(1));
    available_chunks.insert(shard_id_t(3));
    available_chunks.insert(shard_id_t(5));
    available_chunks.insert(shard_id_t(6));
    available_chunks.insert(shard_id_t(7));
    shard_id_set minimum;
    EXPECT_EQ(0, lrc._minimum_to_decode(want_to_read, available_chunks, &minimum));
    EXPECT_EQ(5U, minimum.size());
    EXPECT_EQ(1U, minimum.count(shard_id_t(1)));
    EXPECT_EQ(1U, minimum.count(shard_id_t(3)));
    EXPECT_EQ(1U, minimum.count(shard_id_t(5)));
    EXPECT_EQ(1U, minimum.count(shard_id_t(6)));
    EXPECT_EQ(1U, minimum.count(shard_id_t(7)));

    shard_id_map<bufferlist> decoded(lrc.get_chunk_count());
    EXPECT_EQ(0, lrc._decode(want_to_read, chunks, &decoded));
    {
      string s(chunk_size, 'A');
      EXPECT_EQ(s, string(decoded[shard_id_t(0)].c_str(), chunk_size));
    }
    {
      string s(chunk_size, 'B');
      EXPECT_EQ(s, string(decoded[shard_id_t(1)].c_str(), chunk_size));
    }
    {
      string s(chunk_size, 'C');
      EXPECT_EQ(s, string(decoded[shard_id_t(4)].c_str(), chunk_size));
    }
    {
      string s(chunk_size, 'D');
      EXPECT_EQ(s, string(decoded[shard_id_t(5)].c_str(), chunk_size));
    }
  }
  {
    shard_id_set want_to_read;
    for (unsigned int i = 0; i < lrc.get_chunk_count(); i++)
      want_to_read.insert(shard_id_t(i));
    shard_id_map<bufferlist> chunks(lrc.get_chunk_count());
    chunks[shard_id_t(1)] = encoded[shard_id_t(1)];
    chunks[shard_id_t(3)] = encoded[shard_id_t(3)];
    chunks[shard_id_t(5)] = encoded[shard_id_t(5)];
    chunks[shard_id_t(6)] = encoded[shard_id_t(6)];
    chunks[shard_id_t(7)] = encoded[shard_id_t(7)];
    shard_id_set available_chunks;
    available_chunks.insert(shard_id_t(1));
    available_chunks.insert(shard_id_t(3));
    available_chunks.insert(shard_id_t(5));
    available_chunks.insert(shard_id_t(6));
    available_chunks.insert(shard_id_t(7));
    shard_id_set minimum;
    EXPECT_EQ(0, lrc._minimum_to_decode(want_to_read, available_chunks, &minimum));
    EXPECT_EQ(5U, minimum.size());
    EXPECT_EQ(1U, minimum.count(shard_id_t(1)));
    EXPECT_EQ(1U, minimum.count(shard_id_t(3)));
    EXPECT_EQ(1U, minimum.count(shard_id_t(5)));
    EXPECT_EQ(1U, minimum.count(shard_id_t(6)));
    EXPECT_EQ(1U, minimum.count(shard_id_t(7)));

    shard_id_map<bufferlist> decoded(lrc.get_chunk_count());
    EXPECT_EQ(0, lrc._decode(want_to_read, chunks, &decoded));
    {
      string s(chunk_size, 'A');
      EXPECT_EQ(s, string(decoded[shard_id_t(0)].c_str(), chunk_size));
    }
    {
      string s(chunk_size, 'B');
      EXPECT_EQ(s, string(decoded[shard_id_t(1)].c_str(), chunk_size));
    }
    {
      string s(chunk_size, 'C');
      EXPECT_EQ(s, string(decoded[shard_id_t(4)].c_str(), chunk_size));
    }
    {
      string s(chunk_size, 'D');
      EXPECT_EQ(s, string(decoded[shard_id_t(5)].c_str(), chunk_size));
    }
  }
  {
    shard_id_set want_to_read;
    want_to_read.insert(shard_id_t(6));
    shard_id_map<bufferlist> chunks(lrc.get_chunk_count());
    chunks[shard_id_t(0)] = encoded[shard_id_t(0)];
    chunks[shard_id_t(1)] = encoded[shard_id_t(1)];
    chunks[shard_id_t(3)] = encoded[shard_id_t(3)];
    chunks[shard_id_t(5)] = encoded[shard_id_t(5)];
    chunks[shard_id_t(7)] = encoded[shard_id_t(7)];
    shard_id_set available_chunks;
    available_chunks.insert(shard_id_t(0));
    available_chunks.insert(shard_id_t(1));
    available_chunks.insert(shard_id_t(3));
    available_chunks.insert(shard_id_t(5));
    available_chunks.insert(shard_id_t(7));
    shard_id_set minimum;
    EXPECT_EQ(0, lrc._minimum_to_decode(want_to_read, available_chunks, &minimum));
    EXPECT_EQ(available_chunks, minimum);

    shard_id_map<bufferlist> decoded(lrc.get_chunk_count());
    EXPECT_EQ(0, lrc._decode(want_to_read, chunks, &decoded));
  }
}

/*
 * Local Variables:
 * compile-command: "cd ../.. ;
 *   make -j4 unittest_erasure_code_lrc && valgrind --tool=memcheck \
 *      ./unittest_erasure_code_lrc \
 *      --gtest_filter=*.* --log-to-stderr=true --debug-osd=20"
 * End:
 */
