/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "RenameClassesV2.h"

#include <algorithm>
#include <arpa/inet.h>
#include <map>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>

#include "Warning.h"
#include "Walkers.h"
#include "DexClass.h"
#include "DexInstruction.h"
#include "DexUtil.h"
#include "ReachableClasses.h"
#include "RedexResources.h"

#define MAX_DESCRIPTOR_LENGTH (1024)
#define MAX_IDENT_CHAR (62)
#define BASE MAX_IDENT_CHAR
#define MAX_IDENT (MAX_IDENT_CHAR * MAX_IDENT_CHAR * MAX_IDENT_CHAR)

static const char* METRIC_CLASSES_IN_SCOPE = "num_classes_in_scope";
static const char* METRIC_RENAMED_CLASSES = "**num_renamed**";
static const char* METRIC_MISSING_HIERARCHY_TYPES = "num_missing_hierarchy_types";
static const char* METRIC_MISSING_HIERARCHY_CLASSES = "num_missing_hierarchy_classes";

static RenameClassesPassV2 s_pass;

namespace {

const char* dont_rename_reason_to_metric(DontRenameReasonCode reason) {
  switch (reason) {
    case DontRenameReasonCode::Annotated:
      return "num_dont_rename_annotated";
    case DontRenameReasonCode::Annotations:
      return "num_dont_rename_annotations";
    case DontRenameReasonCode::Specific:
      return "num_dont_rename_specific";
    case DontRenameReasonCode::Packages:
      return "num_dont_rename_packages";
    case DontRenameReasonCode::Hierarchy:
      return "num_dont_rename_hierarchy";
    case DontRenameReasonCode::Resources:
      return "num_dont_rename_resources";
    case DontRenameReasonCode::ClassForNameLiterals:
      return "num_dont_rename_class_for_name_literals";
    case DontRenameReasonCode::Canaries:
      return "num_dont_rename_canaries";
    case DontRenameReasonCode::NativeBindings:
      return "num_dont_rename_native_bindings";
    case DontRenameReasonCode::ClassForTypesWithReflection:
      return "num_dont_rename_class_for_types_with_reflection";
    case DontRenameReasonCode::ProguardCantRename:
      return "num_dont_rename_pg_cant_rename";
    default:
      always_assert_log(false, "Unexpected DontRenameReasonCode: %d", reason);
  }
}

bool dont_rename_reason_to_metric_per_rule(DontRenameReasonCode reason) {
  switch (reason) {
    case DontRenameReasonCode::Annotated:
    case DontRenameReasonCode::Packages:
    case DontRenameReasonCode::Hierarchy:
      return true;
    default:
      return false;
  }
}

void unpackage_private(Scope &scope) {
  walk_methods(scope,
      [&](DexMethod *method) {
        if (is_package_protected(method)) set_public(method);
      });
  walk_fields(scope,
      [&](DexField *field) {
        if (is_package_protected(field)) set_public(field);
      });
  for (auto clazz : scope) {
    if (!clazz->is_external()) {
      set_public(clazz);
    }
  }

  static DexType *dalvikinner =
    DexType::get_type("Ldalvik/annotation/InnerClass;");

  walk_annotations(scope, [&](DexAnnotation* anno) {
    if (anno->type() != dalvikinner) return;
    auto elems = anno->anno_elems();
    for (auto elem : elems) {
      // Fix access flags on all @InnerClass annotations
      if (!strcmp("accessFlags", elem.string->c_str())) {
        always_assert(elem.encoded_value->evtype() == DEVT_INT);
        elem.encoded_value->value((elem.encoded_value->value() & ~VISIBILITY_MASK) | ACC_PUBLIC);
        TRACE(RENAME, 3, "Fix InnerClass accessFlags %s => %08x\n", elem.string->c_str(), elem.encoded_value->value());
      }
    }
  });
}

static char getident(int num) {
  assert(num >= 0 && num < BASE);
  if (num < 10) {
    return '0' + num;
  } else if (num >= 10 && num < 36){
    return 'A' + num - 10;
  } else {
    return 'a' + num - 26 - 10;
  }
}

void get_next_ident(char *out, int num) {
  int low = num;
  int mid = (num / BASE);
  int top = (mid / BASE);
  always_assert_log(num <= MAX_IDENT,
                    "Bailing, Ident %d, greater than maximum\n", num);
  if (top) {
    *out++ = getident(top);
    low -= (top * BASE * BASE);
  }
  if (mid) {
    mid -= (top * BASE);
    *out++ = getident(mid);
    low -= (mid * BASE);
  }
  *out++ = getident(low);
  *out++ = '\0';
}

static int s_base_strings_size = 0;
static int s_ren_strings_size = 0;
static int s_sequence = 0;
static int s_padding = 0;

/**
 * Determine if the given dex item has the given annotation
 *
 * @param t The dex item whose annotations we'll examine
 * @param anno_type The annotatin we're looking for, expressed as DexType
 * @return true IFF dex item t is annotated with anno_type
 */
template<typename T>
bool has_anno(const T* t, const DexType* anno_type) {
  if (anno_type == nullptr) return false;
  if (t->get_anno_set() == nullptr) return false;
  for (const auto& anno : t->get_anno_set()->get_annotations()) {
    if (anno->type() == anno_type) {
      return true;
    }
  }
  return false;
}

}

void RenameClassesPassV2::build_dont_rename_resources(PassManager& mgr, std::set<std::string>& dont_rename_resources) {
  const Json::Value& config = mgr.get_config();
  PassConfig pc(config);
  std::string apk_dir;
  pc.get("apk_dir", "", apk_dir);

  if (apk_dir.size()) {
    // Classes present in manifest
    std::string manifest = apk_dir + std::string("/AndroidManifest.xml");
    for (std::string classname : get_manifest_classes(manifest)) {
      TRACE(RENAME, 4, "manifest: %s\n", classname.c_str());
      dont_rename_resources.insert(classname);
    }

    // Classes present in XML layouts
    for (std::string classname : get_layout_classes(apk_dir)) {
      TRACE(RENAME, 4, "xml_layout: %s\n", classname.c_str());
      dont_rename_resources.insert(classname);
    }

    // Classnames present in native libraries (lib/*/*.so)
    for (std::string classname : get_native_classes(apk_dir)) {
      auto type = DexType::get_type(classname.c_str());
      if (type == nullptr) continue;
      TRACE(RENAME, 4, "native_lib: %s\n", classname.c_str());
      dont_rename_resources.insert(classname);
    }
  }
}

void RenameClassesPassV2::build_dont_rename_class_for_name_literals(
    Scope& scope, std::set<std::string>& dont_rename_class_for_name_literals) {
  // Gather Class.forName literals
  auto match = std::make_tuple(
    m::const_string(/* const-string {vX}, <any string> */),
    m::invoke_static(/* invoke-static {vX}, java.lang.Class;.forName */
      m::opcode_method(m::named<DexMethod>("forName") && m::on_class<DexMethod>("Ljava/lang/Class;"))
      && m::has_n_args(1))
  );

  walk_matching_opcodes(scope, match, [&](const DexMethod*, size_t, DexInstruction** insns){
    DexOpcodeString* const_string = (DexOpcodeString*)insns[0];
    DexOpcodeMethod* invoke_static = (DexOpcodeMethod*)insns[1];
    // Make sure that the registers agree
    if (const_string->dest() == invoke_static->src(0)) {
      auto classname = JavaNameUtil::external_to_internal(
          const_string->get_string()->c_str());
      TRACE(RENAME, 4, "Found Class.forName of: %s, marking %s reachable\n",
        const_string->get_string()->c_str(), classname.c_str());
      dont_rename_class_for_name_literals.insert(classname);
    }
  });
}

void RenameClassesPassV2::build_dont_rename_for_types_with_reflection(
    Scope& scope,
    const ProguardMap& pg_map,
    std::set<std::string>& dont_rename_class_for_types_with_reflection) {

    std::set<DexType*> refl_map;
    for (auto const& refl_type_str : m_dont_rename_types_with_reflection) {
      auto deobf_cls_string = pg_map.translate_class(refl_type_str);
      TRACE(RENAME, 4, "%s got translated to %s\n",
          refl_type_str.c_str(),
          deobf_cls_string.c_str());
      if (deobf_cls_string == "") {
        deobf_cls_string = refl_type_str;
      }
      DexType* type_with_refl = DexType::get_type(deobf_cls_string.c_str());
      if (type_with_refl != nullptr) {
        TRACE(RENAME, 4, "got DexType %s\n", SHOW(type_with_refl));
        refl_map.insert(type_with_refl);
      }
    }

  walk_opcodes(scope,
      [](DexMethod*) { return true; },
      [&](DexMethod* m, DexInstruction* insn) {
        if (insn->has_methods()) {
          auto methodop = static_cast<DexOpcodeMethod*>(insn);
          auto callee = methodop->get_method();
          if (callee == nullptr || !callee->is_concrete()) return;
          auto callee_method_cls = callee->get_class();
          if (refl_map.count(callee_method_cls) == 0) return;
          std::string classname(m->get_class()->get_name()->c_str());
          TRACE(RENAME, 4,
            "Found %s with known reflection usage. marking reachable\n",
            classname.c_str());
          dont_rename_class_for_types_with_reflection.insert(classname);
        }
  });
}

void RenameClassesPassV2::build_dont_rename_canaries(Scope& scope,std::set<std::string>& dont_rename_canaries) {
  // Gather canaries
  for(auto clazz: scope) {
    if(strstr(clazz->get_name()->c_str(), "/Canary")) {
      dont_rename_canaries.insert(std::string(clazz->get_name()->c_str()));
    }
  }
}

void RenameClassesPassV2::build_dont_rename_hierarchies(
    PassManager& mgr,
    Scope& scope,
    std::unordered_map<const DexType*, std::string>& dont_rename_hierarchies) {
  std::vector<DexClass*> base_classes;
  for (const auto& base : m_dont_rename_hierarchies) {
    // skip comments
    if (base.c_str()[0] == '#') continue;
    auto base_type = DexType::get_type(base.c_str());
    if (base_type != nullptr) {
      DexClass* base_class = type_class(base_type);
      if (!base_class) {
        TRACE(RENAME, 2, "Can't find class for dont_rename_hierachy rule %s\n",
              base.c_str());
        mgr.incr_metric(METRIC_MISSING_HIERARCHY_CLASSES, 1);
      } else {
        base_classes.emplace_back(base_class);
      }
    } else {
      TRACE(RENAME, 2, "Can't find type for dont_rename_hierachy rule %s\n",
            base.c_str());
      mgr.incr_metric(METRIC_MISSING_HIERARCHY_TYPES, 1);
    }
  }
  for (const auto& base_class : base_classes) {
    auto base_name = base_class->get_name()->c_str();
    dont_rename_hierarchies[base_class->get_type()] = base_name;
    std::unordered_set<const DexType*> children_and_implementors;
    get_all_children_and_implementors(
        scope, base_class, &children_and_implementors);
    for (const auto& cls : children_and_implementors) {
      dont_rename_hierarchies[cls] = base_name;
    }
  }
}

void RenameClassesPassV2::build_dont_rename_native_bindings(
  Scope& scope,
  std::set<DexType*>& dont_rename_native_bindings) {
  // find all classes with native methods, and all types mentioned in protos of native methods
  for(auto clazz: scope) {
    for (auto meth : clazz->get_dmethods()) {
      if (meth->get_access() & ACC_NATIVE) {
        dont_rename_native_bindings.insert(clazz->get_type());
        auto proto = meth->get_proto();
        auto rtype = proto->get_rtype();
        dont_rename_native_bindings.insert(rtype);
        for (auto ptype : proto->get_args()->get_type_list()) {
          // TODO: techincally we should recurse for array types not just go one level
          if (is_array(ptype)) {
            dont_rename_native_bindings.insert(get_array_type(ptype));
          } else {
            dont_rename_native_bindings.insert(ptype);
          }
        }
      }
    }
    for (auto meth : clazz->get_vmethods()) {
      if (meth->get_access() & ACC_NATIVE) {
        dont_rename_native_bindings.insert(clazz->get_type());
        auto proto = meth->get_proto();
        auto rtype = proto->get_rtype();
        dont_rename_native_bindings.insert(rtype);
        for (auto ptype : proto->get_args()->get_type_list()) {
          // TODO: techincally we should recurse for array types not just go one level
          if (is_array(ptype)) {
            dont_rename_native_bindings.insert(get_array_type(ptype));
          } else {
            dont_rename_native_bindings.insert(ptype);
          }
        }
      }
    }
  }
}

void RenameClassesPassV2::build_dont_rename_annotated(
    std::set<DexType*, dextypes_comparator>& dont_rename_annotated) {
  for (const auto& annotation : m_dont_rename_annotated) {
    DexType *anno = DexType::get_type(annotation.c_str());
    if (anno) {
      dont_rename_annotated.insert(anno);
    }
  }
}

class AliasMap {
  std::map<DexString*, DexString*> m_class_name_map;
  std::map<DexString*, DexString*> m_extras_map;
 public:
  void add_class_alias(DexClass* cls, DexString* alias) {
    m_class_name_map.emplace(cls->get_name(), alias);
  }
  void add_alias(DexString* original, DexString* alias) {
    m_extras_map.emplace(original, alias);
  }
  bool has(DexString* key) const {
    return m_class_name_map.count(key) || m_extras_map.count(key);
  }
  DexString* at(DexString* key) const {
    auto it = m_class_name_map.find(key);
    if (it != m_class_name_map.end()) {
      return it->second;
    }
    return m_extras_map.at(key);
  }
  const std::map<DexString*, DexString*>& get_class_map() const {
    return m_class_name_map;
  }
};

static void sanity_check(const Scope& scope, const AliasMap& aliases) {
  std::unordered_set<std::string> external_names;
  // Class.forName() expects strings of the form "foo.bar.Baz". We should be
  // very suspicious if we see these strings in the string pool that
  // correspond to the old name of a class that we have renamed...
  for (const auto& it : aliases.get_class_map()) {
    external_names.emplace(
        JavaNameUtil::internal_to_external(it.first->c_str()));
  }
  std::vector<DexString*> all_strings;
  for (auto clazz : scope) {
    clazz->gather_strings(all_strings);
  }
  sort_unique(all_strings);
  int sketchy_strings = 0;
  for (auto s : all_strings) {
    if (external_names.find(s->c_str()) != external_names.end() ||
        aliases.has(s)) {
      TRACE(RENAME, 2, "Found %s in string pool after renaming\n", s->c_str());
      sketchy_strings++;
    }
  }
  if (sketchy_strings > 0) {
    fprintf(stderr,
            "WARNING: Found a number of sketchy class-like strings after class "
            "renaming. Re-run with TRACE=RENAME:2 for more details.");
  }
}


void RenameClassesPassV2::eval_classes(
    Scope& scope,
    ConfigFiles& cfg,
    const std::string& path,
    bool rename_annotations,
    PassManager& mgr) {
  std::set<std::string> dont_rename_class_for_name_literals;
  std::set<std::string> dont_rename_class_for_types_with_reflection;
  std::set<std::string> dont_rename_canaries;
  std::set<std::string> dont_rename_resources;
  std::unordered_map<const DexType*, std::string> dont_rename_hierarchies;
  std::set<DexType*> dont_rename_native_bindings;
  std::set<DexType*, dextypes_comparator> dont_rename_annotated;

  build_dont_rename_resources(mgr, dont_rename_resources);
  build_dont_rename_class_for_name_literals(scope, dont_rename_class_for_name_literals);
  build_dont_rename_for_types_with_reflection(scope, cfg.get_proguard_map(),
    dont_rename_class_for_types_with_reflection);
  build_dont_rename_canaries(scope, dont_rename_canaries);
  build_dont_rename_hierarchies(mgr, scope, dont_rename_hierarchies);
  build_dont_rename_native_bindings(scope, dont_rename_native_bindings);
  build_dont_rename_annotated(dont_rename_annotated);

  std::string norule = "";

  for(auto clazz: scope) {
    // Don't rename annotations
    if (!rename_annotations && is_annotation(clazz)) {
      m_dont_rename_reasons[clazz] = { DontRenameReasonCode::Annotations, norule };
      continue;
    }

    // Don't rename types annotated with anything in dont_rename_annotated
    bool annotated = false;
    for (const auto& anno : dont_rename_annotated) {
      if (has_anno(clazz, anno)) {
        m_dont_rename_reasons[clazz] = { DontRenameReasonCode::Annotated, std::string(anno->c_str()) };
        annotated = true;
        break;
      }
    }
    if (annotated) continue;

    const char* clsname = clazz->get_name()->c_str();
    std::string strname = std::string(clsname);

    // Don't rename anything mentioned in resources
    if (dont_rename_resources.count(clsname) > 0) {
      m_dont_rename_reasons[clazz] = { DontRenameReasonCode::Resources, norule };
      continue;
    }

    // Don't rename anythings in the direct name blacklist (hierarchy ignored)
    if (m_dont_rename_specific.count(clsname) > 0) {
      m_dont_rename_reasons[clazz] = { DontRenameReasonCode::Specific, strname };
      continue;
    }

    // Don't rename anything if it falls in a blacklisted package
    bool package_blacklisted = false;
    for (const auto& pkg : m_dont_rename_packages) {
      if (strname.rfind("L"+pkg) == 0) {
        TRACE(RENAME, 2, "%s blacklisted by pkg rule %s\n", clsname, pkg.c_str());
        m_dont_rename_reasons[clazz] = { DontRenameReasonCode::Packages, pkg };
        package_blacklisted = true;
        break;
      }
    }
    if (package_blacklisted) continue;

    if (dont_rename_class_for_name_literals.count(clsname) > 0) {
      m_dont_rename_reasons[clazz] = { DontRenameReasonCode::ClassForNameLiterals, norule };
      continue;
    }

    if (dont_rename_class_for_types_with_reflection.count(clsname) > 0) {
      m_dont_rename_reasons[clazz] = { DontRenameReasonCode::ClassForTypesWithReflection, norule };
      continue;
    }

    if (dont_rename_canaries.count(clsname) > 0) {
      m_dont_rename_reasons[clazz] = { DontRenameReasonCode::Canaries, norule };
      continue;
    }

    // Don't rename things with native bindings
    if (dont_rename_native_bindings.count(clazz->get_type()) > 0) {
      m_dont_rename_reasons[clazz] = { DontRenameReasonCode::NativeBindings, norule };
      continue;
    }

    if (dont_rename_hierarchies.count(clazz->get_type()) > 0) {
      std::string rule = dont_rename_hierarchies[clazz->get_type()];
      m_dont_rename_reasons[clazz] = { DontRenameReasonCode::Hierarchy, rule };
      continue;
    }

    if (!can_rename_if_ignoring_blanket_keep(clazz)) {
      m_dont_rename_reasons[clazz] = { DontRenameReasonCode::ProguardCantRename, norule };
      continue;
    }
  }
}

void RenameClassesPassV2::eval_pass(DexStoresVector& stores, ConfigFiles& cfg, PassManager& mgr) {
  auto scope = build_class_scope(stores);
  eval_classes(scope, cfg, m_path, m_rename_annotations, mgr);
}

void RenameClassesPassV2::rename_classes(
    Scope& scope,
    ConfigFiles& cfg,
    const std::string& path,
    bool rename_annotations,
    PassManager& mgr) {
  // Make everything public
  unpackage_private(scope);

  AliasMap aliases;
  for(auto clazz: scope) {
    auto dtype = clazz->get_type();
    auto oldname = dtype->get_name();

    if (m_dont_rename_reasons.find(clazz) != m_dont_rename_reasons.end()) {
      auto reason = m_dont_rename_reasons[clazz];
      std::string metric = dont_rename_reason_to_metric(reason.code);
      mgr.incr_metric(metric, 1);
      if (dont_rename_reason_to_metric_per_rule(reason.code)) {
        std::string str = metric + "::" + std::string(reason.rule);
        mgr.incr_metric(str, 1);
        TRACE(RENAME, 2, "'%s' NOT RENAMED due to %s'\n", oldname->c_str(), str.c_str());
      } else {
        TRACE(RENAME, 2, "'%s' NOT RENAMED due to %s'\n", oldname->c_str(), metric.c_str());
      }
      continue;
    }

    mgr.incr_metric(METRIC_RENAMED_CLASSES, 1);

    char clzname[4];
    const char* padding = "0000000000000";
    get_next_ident(clzname, s_sequence);
    // The X helps our hacked Dalvik classloader recognize that a
    // class name is the output of the redex renamer and thus will
    // never be found in the Android platform.
    char descriptor[MAX_DESCRIPTOR_LENGTH];
    always_assert((s_padding + strlen("LX/;") + 1) < MAX_DESCRIPTOR_LENGTH);
    sprintf(descriptor, "LX/%.*s%s;",
        (s_padding < (int)strlen(clzname)) ? 0 : s_padding - (int)strlen(clzname),
        padding,
        clzname);
    s_sequence++;

    auto exists = DexString::get_string(descriptor);
    always_assert_log(
        !exists, "Collision on class %s (%s)", oldname->c_str(), descriptor);

    auto dstring = DexString::make_string(descriptor);
    aliases.add_class_alias(clazz, dstring);
    dtype->assign_name_alias(dstring);
    std::string old_str(oldname->c_str());
    std::string new_str(descriptor);
//    proguard_map.update_class_mapping(old_str, new_str);
    s_base_strings_size += strlen(oldname->c_str());
    s_ren_strings_size += strlen(dstring->c_str());

    TRACE(RENAME, 2, "'%s' ->  %s'\n", oldname->c_str(), descriptor);
    while (1) {
     std::string arrayop("[");
      arrayop += oldname->c_str();
      oldname = DexString::get_string(arrayop.c_str());
      if (oldname == nullptr) {
        break;
      }
      auto arraytype = DexType::get_type(oldname);
      if (arraytype == nullptr) {
        break;
      }
      std::string newarraytype("[");
      newarraytype += dstring->c_str();
      dstring = DexString::make_string(newarraytype.c_str());

      aliases.add_alias(oldname, dstring);
      arraytype->assign_name_alias(dstring);
    }
  }

  /* Now we need to re-write the Signature annotations.  They use
   * Strings rather than Type's, so they have to be explicitly
   * handled.
   */

  /* In Signature annotations, parameterized types of the form Foo<Bar> get
   * represented as the strings
   *   "Lcom/baz/Foo" "<" "Lcom/baz/Bar;" ">"
   *
   * Note that "Lcom/baz/Foo" lacks a trailing semicolon.
   * So, we have to alias those strings if they exist. Signature annotations
   * suck.
   */
  for (const auto& apair : aliases.get_class_map()) {
    char buf[MAX_DESCRIPTOR_LENGTH];
    const char *sourcestr = apair.first->c_str();
    size_t sourcelen = strlen(sourcestr);
    if (sourcestr[sourcelen - 1] != ';') continue;
    strcpy(buf, sourcestr);
    buf[sourcelen - 1] = '\0';
    auto dstring = DexString::get_string(buf);
    if (dstring == nullptr) continue;
    strcpy(buf, apair.second->c_str());
    buf[strlen(apair.second->c_str()) - 1] = '\0';
    auto target = DexString::make_string(buf);
    aliases.add_alias(dstring, target);
  }
  static DexType *dalviksig =
    DexType::get_type("Ldalvik/annotation/Signature;");
  walk_annotations(scope, [&](DexAnnotation* anno) {
    if (anno->type() != dalviksig) return;
    auto elems = anno->anno_elems();
    for (auto elem : elems) {
      auto ev = elem.encoded_value;
      if (ev->evtype() != DEVT_ARRAY) continue;
      auto arrayev = static_cast<DexEncodedValueArray*>(ev);
      auto const& evs = arrayev->evalues();
      for (auto strev : *evs) {
        if (strev->evtype() != DEVT_STRING) continue;
        auto stringev = static_cast<DexEncodedValueString*>(strev);
        if (aliases.has(stringev->string())) {
          TRACE(RENAME, 5, "Rewriting Signature from '%s' to '%s'\n",
              stringev->string()->c_str(),
              aliases.at(stringev->string())->c_str());
          stringev->string(aliases.at(stringev->string()));
        }
      }
    }
  });

  if (!path.empty()) {
    FILE* fd = fopen(path.c_str(), "w");
    always_assert_log(fd, "Error writing rename file");
    // record for later processing and back map generation
    for (const auto& it : aliases.get_class_map()) {
      auto cls = type_class(DexType::get_type(it.first));
      fprintf(fd, "%s -> %s\n",
              cls->get_deobfuscated_name().c_str(),
              it.second->c_str());
    }
    fclose(fd);
  }

  for (auto clazz : scope) {
    clazz->get_vmethods().sort(compare_dexmethods);
    clazz->get_dmethods().sort(compare_dexmethods);
    clazz->get_sfields().sort(compare_dexfields);
    clazz->get_ifields().sort(compare_dexfields);
  }

  sanity_check(scope, aliases);
}

void RenameClassesPassV2::run_pass(DexStoresVector& stores, ConfigFiles& cfg, PassManager& mgr) {
  auto scope = build_class_scope(stores);
  int total_classes = scope.size();

  s_base_strings_size = 0;
  s_ren_strings_size = 0;
  s_sequence = 0;
  // encode the whole sequence as base 62, [0 - 9 + a - z + A - Z]
  s_padding = std::ceil(std::log(total_classes) / std::log(BASE));

  m_path = cfg.metafile(m_path);
  rename_classes(scope, cfg, m_path, m_rename_annotations, mgr);

  mgr.incr_metric(METRIC_CLASSES_IN_SCOPE, total_classes);

  TRACE(RENAME, 1, "Total classes in scope for renaming: %d chosen padding: %d\n",
      total_classes, s_padding);
  TRACE(RENAME, 1, "String savings, at least %d-%d = %d bytes \n",
      s_base_strings_size, s_ren_strings_size, s_base_strings_size - s_ren_strings_size);
}
