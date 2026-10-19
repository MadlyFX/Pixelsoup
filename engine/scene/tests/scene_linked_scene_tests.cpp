#include <mediaserver/scene/linked_scene.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_project.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#include <mediaserver/persist/project_file.hpp>
#include <mediaserver/persist/settings.hpp>
#include <mediaserver/cmd/director_scheduler.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace mediaserver;
namespace s = scene::schema;
namespace l = scene::link_schema;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
doc::NodeId id(core::u64 value) { return doc::NodeId{100'000 + value}; }
doc::Node node(core::u64 value, std::string_view type) { return {id(value), std::string{type}, {}}; }
void put(doc::Node& n, std::string_view key, doc::PropertyValue value) { n.properties[std::string{key}] = std::move(value); }
void ref(doc::Node& n, std::string_view key, core::u64 value) { put(n, key, static_cast<core::i64>(id(value).value())); }
doc::Root take(scene::LinkedPlanResult result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}
scene::LinkedSceneState read(const doc::Root& root) {
    auto result = scene::read_linked_scene(root);
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}
bool near(const scene::Mat4& a, const scene::Mat4& b) {
    for (std::size_t i = 0; i < 16; ++i) if (std::abs(a.m[i] - b.m[i]) > 2e-5F) return false;
    return true;
}
doc::NodeId occurrence(const doc::Root& root, core::u64 link, core::u64 object) {
    for (const auto& [n, placed] : read(root).occurrences)
        if (placed.instance == id(link) && placed.object == id(object)) return n;
    throw std::runtime_error("missing occurrence");
}
doc::Root fixture(std::string source = "prop-r1.glb") {
    doc::Root root;
    auto stage = node(1, s::stage_type); put(stage, s::scene_name, std::string{"Blender linked scene"}); root = root.set(stage);
    auto src = node(100, l::source_type); put(src, l::version, core::i64{1}); put(src, l::uuid, scene::link_instance_uuid(id(100))); root = root.set(src);
    auto mesh = node(101, l::prototype_type); ref(mesh, l::source, 100); put(mesh, l::uuid, scene::link_instance_uuid(id(101)));
    put(mesh, l::revision, std::string{"mesh-r1"}); put(mesh, s::source, source); root = root.set(mesh);
    auto mat = node(102, l::material_type); ref(mat, l::source, 100); put(mat, l::uuid, scene::link_instance_uuid(id(102)));
    put(mat, l::revision, std::string{"material-r1"}); put(mat, s::roughness, 0.65); root = root.set(mat);
    auto parent = node(110, l::object_type); ref(parent, l::source, 100); put(parent, l::uuid, scene::link_instance_uuid(id(110)));
    put(parent, s::group_name, std::string{"Placement parent"});
    scene::write_link_matrix(parent, l::local, scene::multiply(scene::translation({1,0,0}), scene::rotation(0.25F,0,0)));
    root = root.set(parent);
    auto hero = node(111, l::object_type); ref(hero, l::source, 100); ref(hero, l::parent, 110); ref(hero, l::prototype, 101); ref(hero, l::material, 102);
    put(hero, l::uuid, scene::link_instance_uuid(id(111))); put(hero, s::group_name, std::string{"Hero prop"});
    scene::write_link_matrix(hero, l::local, scene::multiply(scene::translation({1,1,0}), scene::scaling({2,0.5F,1})));
    root = root.set(hero);
    auto shared = hero; shared.id = id(112); shared.properties.erase(std::string{l::parent});
    put(shared, l::uuid, scene::link_instance_uuid(id(112))); put(shared, s::group_name, std::string{"Shared prop"});
    scene::write_link_matrix(shared, l::local, scene::translation({-2,0.5F,0})); root = root.set(shared);
    return take(scene::plan_attach_link(root, id(100), id(1), id(200)));
}
void tests() {
    auto root = fixture();
    const auto hero = occurrence(root, 200, 111), parent = occurrence(root, 200, 110), shared = occurrence(root, 200, 112);
    auto state = read(root);
    check(near(state.occurrences.at(hero).world, scene::multiply(scene::translation({1,1,-1}),
        scene::multiply(scene::rotation(0.25F,0,0),scene::scaling({2,0.5F,1})))), "reference world oracle");
    root = take(scene::plan_attach_link(root, id(100), id(1), id(300), scene::translation({5,0,0})));
    const auto hero2 = occurrence(root,300,111);
    const auto original = root;
    auto desired = read(root).occurrences.at(hero).world; desired.m[12] += 0.5F;
    root = take(scene::plan_linked_world(root,hero,desired));
    root = take(scene::plan_linked_edit(root,hero,scene::LinkedEdit::Hold));
    auto shot = *root.get(hero); put(shot,s::roughness,0.2); root = take(scene::plan_linked_change(root,root.set(shot)));
    root = take(scene::plan_linked_edit(root,shared,scene::LinkedEdit::Hide));
    const auto shot_root = root;
    auto source_parent = *root.get(id(110)); scene::write_link_matrix(source_parent,l::local,scene::translation({3,0,0}));
    auto mesh = *root.get(id(101)); put(mesh,l::revision,std::string{"mesh-r2"}); put(mesh,s::source,std::string{"prop-r2.glb"});
    auto material = *root.get(id(102)); put(material,l::revision,std::string{"material-r2"}); put(material,s::roughness,0.8);
    auto source_hero = *root.get(id(111)); put(source_hero,s::group_name,std::string{"Hero renamed"}); put(source_hero,s::group_order,core::i64{99});
    const std::array update{source_parent,mesh,material,source_hero};
    root = take(scene::plan_link_source_update(root,update,id(400)));
    state = read(root);
    check(near(state.occurrences.at(hero).world,desired),"hold compensates upstream parent change");
    check(state.occurrences.at(hero2).world != read(shot_root).occurrences.at(hero2).world,"second link follows source independently");
    check(state.occurrences.at(hero).source == "prop-r2.glb" && state.occurrences.at(hero2).source == "prop-r2.glb","shared prototype reaches both links");
    check(state.occurrences.at(hero).material.roughness == 0.2F && state.occurrences.at(hero2).material.roughness == 0.8F,"property ownership survives material revision");
    check(!state.occurrences.at(shared).visible,"source update preserves local hide");
    check(state.occurrences.at(hero).name == "Hero renamed","rename retains document occurrence");
    auto follow = take(scene::plan_linked_edit(root,hero,scene::LinkedEdit::Follow));
    check(near(read(follow).occurrences.at(hero).world,desired),"switch follow captures offset without jumping");
    auto aligned = take(scene::plan_link_alignment(root,id(200),scene::translation({9,2,0})));
    check(near(read(aligned).occurrences.at(hero).world,desired),"hold is world space including alignment");
    auto moved_parent = read(root).occurrences.at(parent).world; moved_parent.m[13] = 3;
    check(near(read(take(scene::plan_linked_world(root,parent,moved_parent))).occurrences.at(hero).world,desired),"hold compensates shot parent edits");
    source_hero.properties.erase(std::string{l::parent});
    root = take(scene::plan_link_source_update(root,std::array{source_hero},id(400)));
    check(near(read(root).occurrences.at(hero).world,desired),"reparent preserves held world");
    const auto before_remove = root;
    put(source_hero,l::removed,true); root = take(scene::plan_link_source_update(root,std::array{source_hero},id(400)));
    check(root.contains(hero) && read(root).occurrences.at(hero).removed && !read(root).occurrences.at(hero).visible,"source tombstone retains recoverable shot");
    check(scene::plan_linked_edit(root,hero,scene::LinkedEdit::ForgetRemoved).has_value(),"deliberate orphan resolution");
    put(source_hero,l::removed,false); root = take(scene::plan_link_source_update(root,std::array{source_hero},id(400)));
    check(near(read(root).occurrences.at(hero).world,desired) && read(root).occurrences.at(hero).material.roughness==0.2F,"restore recovers same identity and shot");
    auto duplicate = take(scene::plan_linked_edit(root,hero,scene::LinkedEdit::DuplicateLink));
    check(read(duplicate).occurrences.size()==9,"duplicate link includes independent copies of shot records");
    auto new_object = source_hero; new_object.id=id(120); put(new_object,l::uuid,scene::link_instance_uuid(id(120)));
    duplicate=take(scene::plan_link_source_update(duplicate,std::array{new_object},id(500)));
    check(read(duplicate).occurrences.size()==12,"new source object creates a persistent occurrence in every link");
    auto cloned = scene::plan_clone_scene(root,id(1),id(600)); check(cloned.has_value(),"Preview clone plans");
    auto preview=root; for(const auto& n:cloned.value().puts) preview=preview.set(n);
    check(read(preview).occurrences.size()==12,"Preview clones link attachment identities and remaps occurrences");
    const auto cloned_hero=*cloned.value().cloned_id(hero);
    preview=take(scene::plan_linked_edit(preview,cloned_hero,scene::LinkedEdit::ResetPlacement));
    check(near(read(preview).occurrences.at(hero).world,desired),"Preview shot edits leave Program alone");
    scene::SceneModel model; model.reset(root); const auto expected=model.evaluate({});
    auto bad_parent=*root.get(id(110)); ref(bad_parent,l::parent,110);
    check(!scene::plan_link_source_update(root,std::array{bad_parent},id(400)),"cycle rejected atomically");
    auto bad=root.set(bad_parent); model.apply(doc::diff(root,bad));
    check(model.linked_error().has_value() && scene::digest(model.evaluate({}))==scene::digest(expected),"invalid snapshot retains scene and digest");
    model.apply(doc::diff(bad,root)); check(!model.linked_error(),"repair resumes after rejected revision");
    bad_parent=*root.get(id(110)); ref(bad_parent,l::parent,999);
    check(!scene::plan_link_source_update(root,std::array{bad_parent},id(400)),"missing parent rejected");
    bad_parent=*root.get(id(110)); scene::write_link_matrix(bad_parent,l::local,scene::scaling({0,1,1}));
    check(!scene::plan_link_source_update(root,std::array{bad_parent},id(400)),"singular transform rejected even for held children");
    bad_parent=*root.get(id(110)); auto nonfinite=scene::identity(); nonfinite.m[12]=std::numeric_limits<float>::infinity(); scene::write_link_matrix(bad_parent,l::local,nonfinite);
    check(!scene::plan_link_source_update(root,std::array{bad_parent},id(400)),"nonfinite matrix rejected");
    bad_parent=*root.get(id(110)); put(bad_parent,"link_local_3",0.01);
    check(!scene::plan_link_source_update(root,std::array{bad_parent},id(400)),"perspective residual rejected");
    bad_parent=*root.get(id(110)); bad_parent.properties.erase("link_local_5");
    check(!scene::plan_link_source_update(root,std::array{bad_parent},id(400)),"partial affine carrier rejected");
    auto bad_identity=source_hero; put(bad_identity,l::uuid,scene::link_instance_uuid(id(112)));
    check(!scene::plan_link_source_update(root,std::array{bad_identity},id(400)),"duplicate UUID rejected");
    auto origin=source_hero; put(origin,l::origin,std::string{"new pivot"});
    check(!scene::plan_link_source_update(root,std::array{origin},id(400)),"uncompensated origin rebase rejected");
    auto animated=node(700,s::channel_type); put(animated,s::channel_target,static_cast<core::i64>(parent.value()));
    check(!scene::plan_linked_change(root,root.set(animated)),"animated parents explicitly rejected");
    bad_parent=*root.get(id(110)); put(bad_parent,s::animation_enabled,true);
    check(!scene::plan_link_source_update(root,std::array{bad_parent},id(400)),"source animation flags rejected");
    auto reflected=scene::scaling({-1,1,1});
    check(!scene::plan_link_alignment(root,id(200),reflected),"reflected linked drawing awaits parity qualification");
    auto degenerate=scene::identity(); degenerate.m[4]=1; degenerate.m[5]=1e-7F;
    check(!scene::plan_link_alignment(root,id(200),degenerate),"near-collinear hierarchy basis rejected");
    auto shear=scene::identity(); shear.m[4]=0.35F;
    auto sheared=take(scene::plan_link_alignment(follow,id(200),shear));
    const auto shear_world=read(sheared).occurrences.at(hero).world;
    sheared=take(scene::plan_linked_edit(sheared,hero,scene::LinkedEdit::Hold));
    sheared=take(scene::plan_linked_edit(sheared,hero,scene::LinkedEdit::Follow));
    check(near(read(sheared).occurrences.at(hero).world,shear_world),"full shear survives world-local conversion");
    const auto snapshot = take(scene::snapshot_link(sheared,id(200)));
    check(!snapshot.get(id(100)) && snapshot.get(id(200))->type == s::group_type,"snapshot has no source dependency");
    const auto detached = take(scene::plan_linked_edit(sheared,id(200),scene::LinkedEdit::Unlink));
    check(!scene::is_linked_occurrence(*detached.get(hero)) && detached.get(id(200))->type == s::group_type,"unlink produces ordinary independent nodes");
    check(detached.get(id(101)) == sheared.get(id(101)),"unlink keeps shared immutable resources for other attachments");
    scene::SceneModel independent; independent.reset(detached);
    const auto detached_scene = independent.evaluate({});
    const auto detached_model = std::find_if(detached_scene.models.begin(),detached_scene.models.end(),[&](const auto& m) {return m.source_node==hero;});
    check(detached_model != detached_scene.models.end() && near(detached_model->world,shear_world),"unlink renders exact affine shear and placement");
    check(detached_model->material_override_mask == read(sheared).occurrences.at(hero).material_mask,"unlink preserves per-slot material override mask");
    const auto copied = take(scene::paste_link_snapshot(detached,snapshot,id(1)));
    independent.reset(copied); check(independent.evaluate({}).models.size() > detached_scene.models.size(),"ordinary paste allocates independent drawables");
    auto remote_mesh = *detached.get(id(101)); put(remote_mesh,s::source,std::string{"later.glb"});
    auto later = take(scene::plan_link_source_update(detached,std::array{remote_mesh},id(900)));
    check(std::get<std::string>(later.get(hero)->properties.at(std::string{s::source})) != "later.glb","future source updates cannot change detached resources");
    auto paused = take(scene::plan_linked_edit(sheared,hero,scene::LinkedEdit::Pause));
    check(std::get<bool>(paused.get(id(200))->properties.at(std::string{l::paused})),"pause belongs to collection");
    auto resumed = take(scene::plan_linked_edit(paused,id(200),scene::LinkedEdit::Resume));
    check(!std::get<bool>(resumed.get(id(200))->properties.at(std::string{l::paused})),"collection resumes");
    cmd::DirectorScheduler detach_history;
    check(detach_history.schedule_document_replace(sheared,cmd::UndoContextId{2},cmd::TransactionId{101},cmd::FrameNumber{1}).has_value(),"seed unlink undo");
    check(detach_history.schedule_document_replace(detached,cmd::UndoContextId{2},cmd::TransactionId{102},cmd::FrameNumber{1}).has_value(),"unlink one transaction");
    check(detach_history.schedule_undo(cmd::UndoContextId{2},cmd::TransactionId{103},cmd::FrameNumber{1}).has_value(),"unlink undo");
    check(doc::diff(detach_history.projected_document(),sheared).changes.empty(),"unlink undo restores entire link and shot layer");
    check(detach_history.schedule_redo(cmd::UndoContextId{2},cmd::TransactionId{104},cmd::FrameNumber{1}).has_value(),"unlink redo");
    check(doc::diff(detach_history.projected_document(),detached).changes.empty(),"unlink redo restores independent appearance");
    const auto path=std::filesystem::temp_directory_path()/("mediaserver-linked-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".mstage");
    check(persist::save_project_file(path,root).has_value(),"persist link project");
    const auto reopened=persist::load_project_file(path); check(reopened.has_value(),"reopen link project");
    scene::SceneModel restored; restored.reset(reopened.value());
    check(scene::digest(restored.evaluate({}))==scene::digest(expected),"persistence/deterministic digest round trip");
    check(persist::save_project_file(path,detached).has_value(),"persist independent accepted resources");
    const auto offline_copy = persist::load_project_file(path);
    check(offline_copy.has_value(),"reopen independent collection without source authoring");
    scene::SceneModel offline_model; offline_model.reset(offline_copy.value());
    check(scene::digest(offline_model.evaluate({}))==scene::digest(detached_scene),"independent affine appearance round trip");
    std::filesystem::remove(path);
    restored.apply(doc::diff(root,before_remove)); restored.apply(doc::diff(before_remove,shot_root)); restored.apply(doc::diff(shot_root,original));
    scene::SceneModel original_model; original_model.reset(original);
    check(restored.evaluate({}).models==original_model.evaluate({}).models,"undo restores identity and overrides through reverse deltas");
    scene::ModelInstance placement; placement.material.roughness=0.2F; placement.material_override_mask=8;
    scene::Material imported; imported.base_color={0.1F,0.8F,0.9F}; imported.base_color_texture="cyan.png";
    const auto merged=scene::model_part_material(placement,imported);
    check(merged.roughness==0.2F && merged.base_color==imported.base_color && merged.base_color_texture=="cyan.png","property override reaches imported slots without replacing their colors/textures");
}
// Media Server edits a linked prop; the artist then edits the same property in
// Blender. The revision carrying the artist's edit absorbs the override in the
// same plan: the scene shows the artist's value, never both stacked, and only
// the named property is touched.
void absorbed_overrides() {
    auto root = fixture();
    const auto hero = occurrence(root, 200, 111), shared = occurrence(root, 200, 112);
    auto moved = read(root).occurrences.at(hero).world; moved.m[12] += 0.5F;
    root = take(scene::plan_linked_world(root, hero, moved));
    root = take(scene::plan_linked_edit(root, hero, scene::LinkedEdit::Hide));
    root = take(scene::plan_linked_edit(root, shared, scene::LinkedEdit::Hold));
    auto tinted = *root.get(hero); put(tinted, s::roughness, 0.2); root = take(scene::plan_linked_change(root, root.set(tinted)));
    // The artist moved the hero in Blender to where Media Server shows it.
    auto source_hero = *root.get(id(111));
    const auto parent_world = read(root).occurrences.at(occurrence(root, 200, 110)).world;
    scene::write_link_matrix(source_hero, l::local, scene::multiply(scene::invert_affine(parent_world), moved));
    root = take(scene::plan_link_source_update(root, std::array{source_hero}, id(500)));
    check(!near(read(root).occurrences.at(hero).world, moved), "source edit without absorb stacks on the override");
    const auto source_uuid = scene::link_instance_uuid(id(100));
    const std::array<std::pair<std::string, std::string>, 1> placement{{{scene::link_instance_uuid(id(111)), "placement"}}};
    root = take(scene::absorb_linked_overrides(root, source_uuid, id(1), placement));
    auto state = read(root);
    check(near(state.occurrences.at(hero).world, moved), "absorbed placement keeps the artist's world");
    check(!state.occurrences.at(hero).visible, "visibility override survives a placement absorb");
    check(state.occurrences.at(hero).material.roughness == 0.2F, "material override survives a placement absorb");
    const std::array<std::pair<std::string, std::string>, 3> rest{{
        {scene::link_instance_uuid(id(111)), "visibility"}, {scene::link_instance_uuid(id(111)), "material"},
        {scene::link_instance_uuid(id(112)), "placement"}}};
    root = take(scene::absorb_linked_overrides(root, source_uuid, id(1), rest));
    state = read(root);
    check(state.occurrences.at(hero).visible && state.occurrences.at(hero).material.roughness != 0.2F, "visibility and material return to the source");
    check(!state.occurrences.at(shared).held, "absorbing a held placement returns it to Follow");
    const std::array<std::pair<std::string, std::string>, 1> unknown{{{scene::link_instance_uuid(id(111)), "teleport"}}};
    check(!scene::absorb_linked_overrides(root, source_uuid, id(1), unknown), "unknown absorbed property refused");
    // A second attachment of the source in the scene: one Blender object cannot
    // show both, so neither override is absorbed.
    root = take(scene::plan_attach_link(root, id(100), id(1), id(600), scene::translation({5,0,0})));
    const auto twin = occurrence(root, 600, 111);
    auto twin_world = read(root).occurrences.at(twin).world; twin_world.m[13] += 1.0F;
    root = take(scene::plan_linked_world(root, twin, twin_world));
    root = take(scene::absorb_linked_overrides(root, source_uuid, id(1), placement));
    check(near(read(root).occurrences.at(twin).world, twin_world), "two attachments keep their own overrides");
}
void deleted_occurrences() {
    auto root = fixture();
    const auto parent = occurrence(root, 200, 110), hero = occurrence(root, 200, 111), shared = occurrence(root, 200, 112);
    auto moved = read(root).occurrences.at(hero).world; moved.m[12] += 0.5F;
    root = take(scene::plan_linked_world(root, hero, moved));
    const auto before = root;
    root = take(scene::plan_linked_edit(root, parent, scene::LinkedEdit::Delete));
    auto state = read(root);
    check(state.occurrences.at(parent).deleted && !state.occurrences.at(parent).visible, "a deleted occurrence is not drawn");
    check(state.occurrences.at(hero).deleted && !state.occurrences.at(hero).visible, "its children are deleted with it");
    check(!state.occurrences.at(shared).deleted && state.occurrences.at(shared).visible, "other objects are untouched");
    check(doc::diff(before, root).changes.size() == 1, "one flag deletes the hierarchy");
    // Blender takes the objects out of its scene; the records stay for Undo.
    auto removed_parent = *root.get(id(110)); put(removed_parent, l::removed, true);
    auto removed_hero = *root.get(id(111)); put(removed_hero, l::removed, true);
    root = take(scene::plan_link_source_update(root, std::array{removed_parent, removed_hero}, id(500)));
    state = read(root);
    check(state.occurrences.at(hero).removed && state.occurrences.at(hero).deleted, "Blender's removal keeps the deleted record");
    // The artist brings them back: that revision absorbs the deletion, and the
    // hero returns with Media Server's placement.
    put(removed_parent, l::removed, false); put(removed_hero, l::removed, false);
    root = take(scene::plan_link_source_update(root, std::array{removed_parent, removed_hero}, id(500)));
    check(read(root).occurrences.at(hero).deleted, "restored source objects stay deleted until absorbed");
    const std::array<std::pair<std::string, std::string>, 1> restored{{{scene::link_instance_uuid(id(110)), "deleted"}}};
    root = take(scene::absorb_linked_overrides(root, scene::link_instance_uuid(id(100)), id(1), restored));
    state = read(root);
    check(!state.occurrences.at(parent).deleted && !state.occurrences.at(hero).deleted && state.occurrences.at(hero).visible,
          "absorbing the deletion restores the hierarchy");
    check(near(state.occurrences.at(hero).world, moved), "a restored object keeps its overrides");
}
void removed_linked_light_is_not_evaluated() {
    auto root = fixture();
    auto source_light = node(113, l::object_type);
    ref(source_light, l::source, 100);
    put(source_light, l::uuid, scene::link_instance_uuid(id(113)));
    put(source_light, s::group_name, std::string{"Work light"});
    put(source_light, s::light_kind, std::string{s::point_kind});
    put(source_light, s::light_unit, std::string{s::lumens_unit});
    put(source_light, s::intensity, 850.0);
    root = take(scene::plan_link_source_update(root, std::array{source_light}, id(500)));
    const auto light = occurrence(root, 200, 113);
    scene::SceneModel model;
    model.reset(root);
    check(model.evaluate(id(1), {}).point_lights.size() == 1, "linked light appears before removal");

    const auto hidden = take(scene::plan_linked_edit(root, light, scene::LinkedEdit::Hide));
    model.apply(doc::diff(root, hidden));
    check(model.evaluate(id(1), {}).point_lights.empty(), "hidden linked light does not illuminate or draw a guide");
    model.apply(doc::diff(hidden, root));
    check(model.evaluate(id(1), {}).point_lights.size() == 1, "show restores linked light");

    const auto deleted = take(scene::plan_linked_edit(root, light, scene::LinkedEdit::Delete));
    model.apply(doc::diff(root, deleted));
    check(model.evaluate(id(1), {}).point_lights.empty(), "deleted linked light is absent from evaluated scene");
    model.apply(doc::diff(deleted, root));

    put(source_light, l::removed, true);
    const auto removed = take(scene::plan_link_source_update(root, std::array{source_light}, id(500)));
    check(removed.contains(light) && read(removed).occurrences.at(light).removed,
          "Blender removal retains the light occurrence for recovery");
    model.apply(doc::diff(root, removed));
    check(model.evaluate(id(1), {}).point_lights.empty(), "Blender removal clears lighting and Stage guide");
    scene::SceneModel rebuilt;
    rebuilt.reset(removed);
    check(rebuilt.evaluate(id(1), {}).point_lights.empty(), "reopened project also omits removed light");
    model.apply(doc::diff(removed, root));
    check(model.evaluate(id(1), {}).point_lights.size() == 1, "source restoration reuses the light identity");
}
void compositing_overrides() {
    auto root = fixture();
    const auto hero = occurrence(root, 200, 111);
    auto shot = *root.get(hero);
    put(shot, s::opacity, 0.4); put(shot, s::blend_mode, static_cast<double>(scene::BlendMode::Add));
    root = take(scene::plan_linked_change(root, root.set(shot)));
    const auto styled = [&](const doc::Root& r) {
        scene::SceneModel model; model.reset(r);
        for (const auto& m : model.evaluate({}).models)
            if (m.source_node == hero) return std::abs(m.style.opacity - 0.4F) < 1e-6F && m.style.blend == scene::BlendMode::Add;
        return false;
    };
    check(styled(root), "a linked model draws with its shot opacity and blend mode");
    auto source_hero = *root.get(id(111)); put(source_hero, s::group_name, std::string{"Hero renamed"});
    root = take(scene::plan_link_source_update(root, std::array{source_hero}, id(400)));
    check(styled(root), "a Blender revision keeps the shot opacity and blend mode");
}
void artifacts(const std::filesystem::path& reference, const std::filesystem::path& output) {
    auto base=persist::load_project_file(reference); check(base.has_value(),"load step 1 UI fixture");
    const auto* model=base.value().get(id(10)); check(model!=nullptr,"reference model exists");
    const auto source=std::get<std::string>(model->properties.at(std::string{s::source}));
    auto root=fixture(source);
    for(const auto& c:doc::diff({},base.value()).changes) {
        if(c.after->type==s::model_type || c.after->type==s::group_type || c.id==id(1)) continue;
        root=root.set(*c.after);
    }
    const auto hero=occurrence(root,200,111);
    auto world=read(root).occurrences.at(hero).world; world.m[12]+=0.5F;
    root=take(scene::plan_linked_world(root,hero,world)); root=take(scene::plan_linked_edit(root,hero,scene::LinkedEdit::Hold));
    std::filesystem::create_directories(output);
    check(persist::save_project_file(output/"linked-r1.mstage",root).has_value(),"save UI r1");
    auto mesh=*root.get(id(101)); put(mesh,l::revision,std::string{"mesh-r2"});
    auto path=std::filesystem::path{source}; path.replace_filename("prop-r2.glb"); put(mesh,s::source,path.string());
    auto parent=*root.get(id(110)); scene::write_link_matrix(parent,l::local,scene::translation({2,0,0}));
    root=take(scene::plan_link_source_update(root,std::array{mesh,parent},id(400)));
    check(persist::save_project_file(output/"linked-r2.mstage",root).has_value(),"save UI r2");
    auto object=*root.get(id(111)); put(object,l::removed,true); auto removed=take(scene::plan_link_source_update(root,std::array{object},id(400)));
    check(persist::save_project_file(output/"source-removed.mstage",removed).has_value(),"save recovery fixture");
    check(persist::save_project_file(output/"source-restored.mstage",root).has_value(),"save restored fixture");
    for (const auto name : {"linked-r1", "linked-r2", "source-removed", "source-restored"})
        check(persist::save_recent_project_path(output/name/"MediaServer/settings/recent-project.bin",
            std::filesystem::absolute(output/(std::string{name}+".mstage"))).has_value(),"isolated UI startup project");
}
} // namespace
int main(int argc,char** argv) {
    try { tests(); absorbed_overrides(); deleted_occurrences(); removed_linked_light_is_not_evaluated(); compositing_overrides(); if(argc==3) artifacts(argv[1],argv[2]); std::cout<<"Linked scene identity, affine ownership, recovery, clone, persistence and digest checks passed\n"; return 0; }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
