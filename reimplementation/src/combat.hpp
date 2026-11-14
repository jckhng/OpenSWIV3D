#pragma once

#include "flight.hpp"

#include <deque>
#include <map>
#include <optional>

namespace swiv3d::detail {

// Owns pool bookkeeping; the canonical world and references remain in MissionState.
class Combat {
public:
    using Height = std::function<std::int32_t(std::int32_t, std::int32_t)>;
    using Resource = std::function<std::vector<std::uint8_t>(std::uint32_t)>;
    using Deform = std::function<void(const EntityState&, int)>;
    using ModelBounds = std::function<void(int, int)>;
    Combat(MissionState& state, Height height, Resource resource, Deform deform = {}, Height surface = {},
           std::optional<CraftKind> initial_craft = std::nullopt, ModelBounds bounds = {});
    void advance(const KeyboardState& keys);
    void apply_cheat(int action);

private:
    enum class NodeKind : std::size_t { water = 6, air = 7, land = 8 };
    EntityState& entity(int slot);
    EntityState& spawn(EntityState& parent, int flags, int health, int model, Behavior behavior);
    void link(EntityState& actor);
    void retire(EntityState& actor);
    void update(EntityState& actor);
    void collide(std::size_t index);
    void search(EntityState& actor, int interval, int radius = 1, int mask = 0x44, int bits = 4);
    void aim(EntityState& actor);
    void fire(EntityState& actor);
    void sound(const EntityState& actor, int id);
    void track_gun(const EntityState& player);
    void pickup(EntityState& actor);
    bool can_give(int grant);
    void give(int grant);
    void primary(EntityState& player);
    void weapon_controls(const KeyboardState& keys);
    void select_weapon(int selection);
    void special_update();
    void special_family();
    void place_weapon(int mount, int x, int y, int z);
    void place(EntityState& parent, EntityState& actor, int x, int y, int z);
    void weapon_actor(EntityState& actor);
    void trail(EntityState& actor, int interval);
    std::vector<int> weapon_targets(EntityState& actor, int radius, int mask = 0x4c);
    void search_weapon_target(EntityState& actor);
    struct ActiveRegion { int actor, passes; };
    std::deque<ActiveRegion> active_regions_;
    void missile(EntityState& actor);
    int face(EntityState& actor, int turn);
    bool steer_towards(EntityState& actor, int x, int z, int turn);
    EntityState& clone(EntityState& actor, Behavior behavior);
    void change_station(EntityState& actor);
    void vehicle_actor(EntityState& actor);
    void vehicle_control(const KeyboardState& keys);
    void finish_vehicle();
    void stop_engine();
    enum class VehicleSetup { initial, changeover };
    void setup_vehicle(CraftKind craft, VehicleSetup setup = VehicleSetup::changeover);
    void place_wheels(int steering_step);
    struct GroundContact { std::array<int, 4> wheels; std::array<int, 3> momentum; int slide_x, slide_z, texture; };
    GroundContact ground_contact(EntityState& actor, int force);
    void settle(EntityState& actor, const GroundContact& contact);
    void reveal_pickup(EntityState& actor);
    enum class ObjectiveFinish { extraction, ground_exit };
    void check_objective(EntityState& actor, ObjectiveFinish finish = ObjectiveFinish::extraction);
    void kill_player(EntityState& player);
    void aircraft(EntityState& actor);
    void demo_helicopter(EntityState& actor);
    void destroy(EntityState& actor);
    void illuminate(const EntityState& actor, int size);
    void ground_blast(EntityState& actor);
    void mission_actor(EntityState& actor);
    void boss(EntityState& actor);
    void later_boss(EntityState& actor);
    void lunar_actor(EntityState& actor);
    void mars_actor(EntityState& actor);
    void balance_height(EntityState& actor, int clearance);
    void follow_node(EntityState& actor, int turn);
    void scan_player(EntityState& actor, int radius_squared);
    void turn_to(EntityState& actor, int x, int z, int turn);
    void align_attitude(EntityState& actor, const EntityState& reference);
    void patrol(EntityState& actor);
    void load_nodes();
    void nearest_node(EntityState& actor, NodeKind kind);
    void award(const EntityState& actor, int score);
    void debris(EntityState& actor);
    EntityState& add_object(EntityState& actor, int flags, int health, int model, Behavior behavior);
    void effect(EntityState& actor, Behavior behavior, int frame);
    void smoke(EntityState& actor);
    void tank(EntityState& actor);
    void change_node(EntityState& actor, NodeKind kind = NodeKind::land);
    int node_distance(int node, const EntityState& target, int previous, int depth, NodeKind kind = NodeKind::land);
    int bound(int model);
    MissionState& state_;
    Height height_;
    Height surface_;
    Resource resource_;
    Deform deform_;
    ModelBounds model_bounds_;
    std::array<int, object_slots + 1> indices_{};
    std::deque<int> free_;
    std::vector<std::array<int, 9>> nodes_;
    std::map<int, std::vector<int>> debris_models_;
    std::map<int, std::size_t> debris_cursors_;
    KeyboardState previous_keys_{};
    PlayerControl booster_control_ = PlayerControl::normal;
};

}
