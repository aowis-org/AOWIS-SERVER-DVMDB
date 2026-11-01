// Review finding F8: no undeclared enum value, NaN, infinity or physically
// impossible number is ever stored or read. Covers the codec (both
// directions), the value rules (AOWIS-SERVER-MODEL/docs/model-value-rules.md) and every
// hydraulic save and edit path, including the indexed fast paths.
#include "commit_metadata_fixture.h"
#include "aowis/dvmdb/hydraulic_network_persistence.h"

#include <aowis/dvmdb/model_codec.h>
#include <aowis/dvmdb/model_validation.h>

#include <QCoreApplication>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

// The generated table the codec uses; included here to check its content.
template<typename T> struct ModelEnumValues;
#include "model_enum_values.inc"

using aowis::dvmdb::test::commitMetadata;

namespace {
namespace native = aowis::dvmdb;
namespace hydraulic = aowis::dvmdb::hydraulic;
using native::ModelCodec;
using native::ModelValidationReport;

constexpr double nan = std::numeric_limits<double>::quiet_NaN();
constexpr double inf = std::numeric_limits<double>::infinity();

void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename T>
QByteArray encoded(const T& value) {
    return ModelCodec<T>::encode(value);
}

// Position of the first differing byte of two encodings of one entity.
qsizetype firstDifference(const QByteArray& left, const QByteArray& right) {
    expect(left.size() == right.size(), "encodings differ in size");
    for (qsizetype index = 0; index < left.size(); ++index)
        if (left[index] != right[index]) return index;
    throw std::runtime_error("encodings are identical");
}

QByteArray withInt32(QByteArray bytes, qsizetype start, std::int32_t value) {
    const std::uint32_t raw = static_cast<std::uint32_t>(value);
    for (int byte = 0; byte < 4; ++byte)
        bytes[start + byte] = static_cast<char>((raw >> (24 - 8 * byte)) & 0xffU);
    return bytes;
}

QByteArray withDouble(QByteArray bytes, qsizetype start, double value) {
    std::uint64_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    for (int byte = 0; byte < 8; ++byte)
        bytes[start + byte] = static_cast<char>((raw >> (56 - 8 * byte)) & 0xffU);
    return bytes;
}

template<typename T>
std::size_t enumeratorCount() {
    return sizeof(ModelEnumValues<T>::values) / sizeof(ModelEnumValues<T>::values[0]);
}

void testGeneratedEnumTable() {
    expect(enumeratorCount<HydraulicLinkValveType>() == 7, "valve types incomplete");
    expect(enumeratorCount<EntityModelRole>() == 7, "model roles incomplete");
    expect(enumeratorCount<MultiSpeciesSourceType>() == 4, "MSX source types incomplete");
    // Aliased enumerators are listed under every name, with values 0 and 1.
    std::set<int> elevation;
    for (const HydraulicNodeElevationInputType value : ModelEnumValues<HydraulicNodeElevationInputType>::values)
        elevation.insert(static_cast<int>(value));
    expect(enumeratorCount<HydraulicNodeElevationInputType>() == 5 && elevation == std::set<int>{0, 1},
           "aliased elevation input enumerators wrong");
}

void testCodecRejectsUndeclaredEnumerators() {
    HydraulicLinkValve valve;
    valve.uuid = QUuid::createUuid();
    valve.type = HydraulicLinkValveType::PCV;
    expect(!encoded(valve).isEmpty(), "valid valve not encoded");
    for (const int raw : {7, -1, 1000}) {
        valve.type = static_cast<HydraulicLinkValveType>(raw);
        expect(encoded(valve).isEmpty(), "undeclared valve type encoded");
    }
    // Enums in nested structs, optionals and alias-typed fields.
    HydraulicNodeJunction junction;
    junction.uuid = QUuid::createUuid();
    junction.metadata.model_role = static_cast<EntityModelRole>(7);
    expect(encoded(junction).isEmpty(), "undeclared metadata role encoded");
    HydraulicControlRule rule;
    rule.uuid = QUuid::createUuid();
    rule.actions_then.append(HydraulicControlRuleAction{});
    rule.actions_then[0].status = static_cast<HydraulicControlRuleStatus>(3);
    expect(encoded(rule).isEmpty(), "undeclared optional rule status encoded");
    HydraulicNodeTank tank;
    tank.uuid = QUuid::createUuid();
    tank.elevation_input_type = static_cast<HydraulicNodeTankElevationInputType>(2);
    expect(encoded(tank).isEmpty(), "undeclared alias enum encoded");
    tank.elevation_input_type = HydraulicNodeTankElevationInputType::TerrainElevationAndOffset;
    expect(!encoded(tank).isEmpty(), "declared alias enumerator refused");

    // Decoding: patch the stored enum of a valid encoding.
    valve.type = HydraulicLinkValveType::PRV;
    const QByteArray first = encoded(valve);
    valve.type = HydraulicLinkValveType::TCV;
    const qsizetype last = firstDifference(first, encoded(valve)); // low byte of the int32
    const qsizetype start = last - 3;
    std::optional<HydraulicLinkValve> decoded = ModelCodec<HydraulicLinkValve>::decode(withInt32(first, start, 6));
    expect(decoded && decoded->type == HydraulicLinkValveType::PCV, "enum patch position wrong");
    for (const std::int32_t raw : {7, -1, std::numeric_limits<std::int32_t>::max()}) {
        QString error;
        expect(!ModelCodec<HydraulicLinkValve>::decode(withInt32(first, start, raw), &error) &&
               error.contains(QStringLiteral("enumerator")), "undeclared stored enum decoded");
    }
}

void testCodecRejectsNonFiniteNumbers() {
    for (const double bad : {nan, inf, -inf}) {
        HydraulicLinkPipe pipe;
        pipe.uuid = QUuid::createUuid();
        pipe.diameter_mm = bad;
        expect(encoded(pipe).isEmpty(), "non-finite diameter encoded");
        pipe.diameter_mm = 100;
        pipe.length_measured_m = bad;
        expect(encoded(pipe).isEmpty(), "non-finite optional length encoded");
        pipe.length_measured_m.reset();
        pipe.vertices.append(HydraulicLinkVertex{});
        pipe.vertices[0].coordinate_wgs84.latitude_deg = bad;
        expect(encoded(pipe).isEmpty(), "non-finite vertex encoded");
        HydraulicPatternTime pattern;
        pattern.uuid = QUuid::createUuid();
        pattern.multipliers = {1.0, bad};
        expect(encoded(pattern).isEmpty(), "non-finite list element encoded");
        NetworkHydraulic network;
        network.uuid = QUuid::createUuid();
        network.options_hydraulic.accuracy = bad;
        expect(encoded(network).isEmpty(), "non-finite network option encoded");
    }
    HydraulicLinkPipe pipe;
    pipe.uuid = QUuid::createUuid();
    pipe.diameter_mm = 2.0;
    const QByteArray positive = encoded(pipe);
    pipe.diameter_mm = -2.0;
    const qsizetype start = firstDifference(positive, encoded(pipe)); // sign byte = first byte
    const std::optional<HydraulicLinkPipe> three =
        ModelCodec<HydraulicLinkPipe>::decode(withDouble(positive, start, 3.0));
    expect(three && three->diameter_mm == 3.0, "double patch position wrong");
    for (const double bad : {nan, inf, -inf}) {
        QString error;
        expect(!ModelCodec<HydraulicLinkPipe>::decode(withDouble(positive, start, bad), &error) &&
               error.contains(QStringLiteral("non-finite")), "non-finite stored double decoded");
    }
}

// Default-constructed entities are drafts and must be valid.
void testDraftDefaultsAreValid() {
    expect(native::validateHydraulicEntityValues(HydraulicPipeMaterial{}).valid() &&
           native::validateHydraulicEntityValues(HydraulicPatternTime{}).valid() &&
           native::validateHydraulicEntityValues(HydraulicCurveTankVolume{}).valid() &&
           native::validateHydraulicEntityValues(HydraulicCurvePumpHead{}).valid() &&
           native::validateHydraulicEntityValues(HydraulicCurvePumpEfficiency{}).valid() &&
           native::validateHydraulicEntityValues(HydraulicCurveValveHeadloss{}).valid() &&
           native::validateHydraulicEntityValues(HydraulicCurveValveCharacteristic{}).valid() &&
           native::validateHydraulicEntityValues(HydraulicCurveGeneric{}).valid() &&
           native::validateHydraulicEntityValues(HydraulicNodeReservoir{}).valid() &&
           native::validateHydraulicEntityValues(HydraulicNodeJunction{}).valid() &&
           native::validateHydraulicEntityValues(HydraulicNodeTank{}).valid() &&
           native::validateHydraulicEntityValues(HydraulicDemandPoint{}).valid() &&
           native::validateHydraulicEntityValues(HydraulicLinkPipe{}).valid() &&
           native::validateHydraulicEntityValues(HydraulicLinkPump{}).valid() &&
           native::validateHydraulicEntityValues(HydraulicLinkValve{}).valid() &&
           native::validateHydraulicEntityValues(HydraulicControlSimple{}).valid() &&
           native::validateHydraulicEntityValues(HydraulicControlRule{}).valid() &&
           native::validateHydraulicEntityValues(HydraulicMapLabel{}).valid(),
           "a default-constructed entity violates the value rules");
    NetworkHydraulic network;
    network.uuid = QUuid::createUuid();
    expect(native::validateHydraulicValues(network).valid() && native::validateHydraulicNetwork(network).valid(),
           "default network violates the value rules");
}

struct Case {
    const char* label;
    std::function<ModelValidationReport()> run;
    const char* path; // expected suffix of the first issue path; nullptr = valid
};

template<typename T>
std::function<ModelValidationReport()> entity(const std::function<void(T&)>& change) {
    return [change] {
        T value{};
        value.uuid = QUuid::createUuid();
        change(value);
        return native::validateHydraulicEntityValues(value);
    };
}

std::function<ModelValidationReport()> network(const std::function<void(NetworkHydraulic&)>& change) {
    return [change] {
        NetworkHydraulic value;
        value.uuid = QUuid::createUuid();
        change(value);
        return native::validateHydraulicValues(value);
    };
}

void testRuleTable() {
    using Pipe = HydraulicLinkPipe;
    using Pump = HydraulicLinkPump;
    using Valve = HydraulicLinkValve;
    using Junction = HydraulicNodeJunction;
    using Tank = HydraulicNodeTank;
    const std::vector<Case> cases{
        // Pipes
        {"pipe diameter 0", entity<Pipe>([](Pipe& p) { p.diameter_mm = 0; }), ".diameter_mm"},
        {"pipe diameter negative", entity<Pipe>([](Pipe& p) { p.diameter_mm = -1; }), ".diameter_mm"},
        {"pipe length negative", entity<Pipe>([](Pipe& p) { p.length_calculated_m = -0.1; }), ".length_calculated_m"},
        {"pipe measured length 0", entity<Pipe>([](Pipe& p) { p.length_measured_m = 0.0; }), ".length_measured_m"},
        {"pipe HW roughness 0", entity<Pipe>([](Pipe& p) { p.roughness_hazen_williams = 0; }), ".roughness_hazen_williams"},
        {"pipe DW roughness negative", entity<Pipe>([](Pipe& p) { p.roughness_darcy_weisbach_mm = -0.01; }), ".roughness_darcy_weisbach_mm"},
        {"pipe CM roughness 0", entity<Pipe>([](Pipe& p) { p.roughness_chezy_manning = 0; }), ".roughness_chezy_manning"},
        {"pipe minor loss negative", entity<Pipe>([](Pipe& p) { p.minor_loss_coefficient = -1; }), ".minor_loss_coefficient"},
        {"pipe leak negative", entity<Pipe>([](Pipe& p) { p.leak_area_mm2_per_100m = -1; }), ".leak_area_mm2_per_100m"},
        {"pipe leak expansion negative", entity<Pipe>([](Pipe& p) { p.leak_area_expansion_per_pressure_head_mm2_per_m = -1; }),
         ".leak_area_expansion_per_pressure_head_mm2_per_m"},
        {"pipe wall order 2", entity<Pipe>([](Pipe& p) { p.wall_reaction.order = 2; }), ".wall_reaction.order"},
        {"pipe bulk NaN", entity<Pipe>([](Pipe& p) { p.bulk_reaction.coefficient = nan; }), ".bulk_reaction.coefficient"},
        {"pipe vertex longitude", entity<Pipe>([](Pipe& p) {
             p.vertices.append(HydraulicLinkVertex{}); p.vertices[0].coordinate_wgs84.longitude_deg = 180.5; }),
         ".vertices[0].coordinate_wgs84.longitude_deg"},
        {"pipe DW roughness 0 (smooth)", entity<Pipe>([](Pipe& p) { p.roughness_darcy_weisbach_mm = 0; }), nullptr},
        {"pipe wall order 0", entity<Pipe>([](Pipe& p) { p.wall_reaction.order = 0; }), nullptr},
        {"pipe negative bulk decay and order", entity<Pipe>([](Pipe& p) {
             p.bulk_reaction.coefficient = -0.5; p.bulk_reaction.order = -1; }), nullptr},
        // Pumps
        {"pump efficiency 0", entity<Pump>([](Pump& p) { p.constant_efficiency_percent = 0; }), ".constant_efficiency_percent"},
        {"pump efficiency 101", entity<Pump>([](Pump& p) { p.constant_efficiency_percent = 101; }), ".constant_efficiency_percent"},
        {"pump efficiency 100", entity<Pump>([](Pump& p) { p.constant_efficiency_percent = 100; }), nullptr},
        {"pump speed negative", entity<Pump>([](Pump& p) { p.initial_speed_ratio = -0.1; }), ".initial_speed_ratio"},
        {"pump power negative", entity<Pump>([](Pump& p) { p.constant_power_kw = -1; }), ".constant_power_kw"},
        {"pump price negative", entity<Pump>([](Pump& p) { p.energy_price_per_kw_h = -1; }), ".energy_price_per_kw_h"},
        // Valves
        {"valve position 101", entity<Valve>([](Valve& v) { v.setting_position_percent = 101; }), ".setting_position_percent"},
        {"valve position -1", entity<Valve>([](Valve& v) { v.setting_position_percent = -1; }), ".setting_position_percent"},
        {"valve position 0 and 100", entity<Valve>([](Valve& v) { v.setting_position_percent = 100; }), nullptr},
        {"valve flow negative", entity<Valve>([](Valve& v) { v.setting_flow_m3_per_h = -1; }), ".setting_flow_m3_per_h"},
        {"valve loss negative", entity<Valve>([](Valve& v) { v.setting_loss_coefficient = -1; }), ".setting_loss_coefficient"},
        {"valve diameter negative", entity<Valve>([](Valve& v) { v.diameter_mm = -1; }), ".diameter_mm"},
        {"valve minor loss negative", entity<Valve>([](Valve& v) { v.minor_loss_coefficient = -1; }), ".minor_loss_coefficient"},
        {"valve pressure infinite", entity<Valve>([](Valve& v) { v.setting_pressure_head_m = inf; }), ".setting_pressure_head_m"},
        // Junctions and reservoirs
        {"junction emitter negative", entity<Junction>([](Junction& j) { j.emitter.coefficient = -1; }), ".emitter.coefficient"},
        {"junction emitter exponent 0", entity<Junction>([](Junction& j) { j.emitter.pressure_exponent = 0; }), ".emitter.pressure_exponent"},
        {"junction concentration negative", entity<Junction>([](Junction& j) { j.initial_chemical_concentration_mg_per_l = -1; }),
         ".initial_chemical_concentration_mg_per_l"},
        {"junction water age negative", entity<Junction>([](Junction& j) { j.initial_water_age_h = -1; }), ".initial_water_age_h"},
        {"junction elevation NaN", entity<Junction>([](Junction& j) { j.elevation_m = nan; }), ".elevation_m"},
        {"junction latitude 90.5", entity<Junction>([](Junction& j) { j.coordinate_wgs84.latitude_deg = 90.5; }),
         ".coordinate_wgs84.latitude_deg"},
        {"junction altitude infinite", entity<Junction>([](Junction& j) { j.coordinate_wgs84.altitude_m = inf; }),
         ".coordinate_wgs84.altitude_m"},
        {"junction demand NaN", entity<Junction>([](Junction& j) {
             j.demands.append(HydraulicDemand{}); j.demands[0].base_demand_m3_per_h = nan; }), ".demands[0].base_demand_m3_per_h"},
        {"junction source mass negative", entity<Junction>([](Junction& j) {
             j.quality_source.chemical_mass_flow_mg_per_min = -1; }), ".quality_source.chemical_mass_flow_mg_per_min"},
        {"junction below sea level, inflow demand", entity<Junction>([](Junction& j) {
             j.elevation_m = -30; j.demands.append(HydraulicDemand{}); j.demands[0].base_demand_m3_per_h = -5;
             j.coordinate_wgs84.longitude_deg = -180; j.coordinate_wgs84.latitude_deg = 90; }), nullptr},
        {"reservoir head infinite", entity<HydraulicNodeReservoir>([](HydraulicNodeReservoir& r) { r.hydraulic_head_m = inf; }),
         ".hydraulic_head_m"},
        // Tanks
        {"tank minimum above initial", entity<Tank>([](Tank& t) {
             t.water_level_minimum_m = 2; t.water_level_initial_m = 1; t.water_level_maximum_m = 5; }), ".water_level_initial_m"},
        {"tank initial above maximum", entity<Tank>([](Tank& t) {
             t.water_level_initial_m = 6; t.water_level_maximum_m = 5; }), ".water_level_initial_m"},
        {"tank negative level", entity<Tank>([](Tank& t) { t.water_level_minimum_m = -1; }), ".water_level_minimum_m"},
        {"tank equal levels", entity<Tank>([](Tank& t) {
             t.water_level_minimum_m = 3; t.water_level_initial_m = 3; t.water_level_maximum_m = 3; }), nullptr},
        {"tank mixing 1.1", entity<Tank>([](Tank& t) { t.mixing_fraction = 1.1; }), ".mixing_fraction"},
        {"tank mixing 0", entity<Tank>([](Tank& t) { t.mixing_fraction = 0; }), nullptr},
        {"tank diameter negative", entity<Tank>([](Tank& t) { t.diameter_m = -1; }), ".diameter_m"},
        {"tank area negative", entity<Tank>([](Tank& t) { t.cross_section_area_m2 = -1; }), ".cross_section_area_m2"},
        {"tank volume negative", entity<Tank>([](Tank& t) { t.volume_at_maximum_level_m3 = -1; }), ".volume_at_maximum_level_m3"},
        {"tank minimum volume negative", entity<Tank>([](Tank& t) { t.minimum_volume_m3 = -1; }), ".minimum_volume_m3"},
        // Curves and patterns
        {"pump curve duplicate flow", entity<HydraulicCurvePumpHead>([](HydraulicCurvePumpHead& c) {
             c.points = {{0, 50}, {10, 40}, {10, 30}}; }), ".points[2]"},
        {"pump curve negative head", entity<HydraulicCurvePumpHead>([](HydraulicCurvePumpHead& c) {
             c.points = {{0, -1}}; }), ".points[0].head_gain_m"},
        {"pump curve valid", entity<HydraulicCurvePumpHead>([](HydraulicCurvePumpHead& c) {
             c.points = {{0, 50}, {10, 40}, {20, 0}}; }), nullptr},
        {"efficiency 101", entity<HydraulicCurvePumpEfficiency>([](HydraulicCurvePumpEfficiency& c) {
             c.points = {{0, 0}, {10, 101}}; }), ".points[1].efficiency_percent"},
        {"tank curve decreasing volume", entity<HydraulicCurveTankVolume>([](HydraulicCurveTankVolume& c) {
             c.points = {{0, 0}, {1, 10}, {2, 5}}; }), ".points[2]"},
        {"tank curve decreasing level", entity<HydraulicCurveTankVolume>([](HydraulicCurveTankVolume& c) {
             c.points = {{1, 0}, {0, 0}}; }), ".points[1]"},
        {"valve characteristic 101", entity<HydraulicCurveValveCharacteristic>([](HydraulicCurveValveCharacteristic& c) {
             c.points = {{0, 0}, {100, 101}}; }), ".points[1].relative_flow_percent"},
        {"valve headloss negative", entity<HydraulicCurveValveHeadloss>([](HydraulicCurveValveHeadloss& c) {
             c.points = {{0, -1}}; }), ".points[0].head_loss_m"},
        {"generic curve NaN", entity<HydraulicCurveGeneric>([](HydraulicCurveGeneric& c) {
             c.points = {{nan, 1}}; }), ".points[0].x"},
        {"generic curve negative values", entity<HydraulicCurveGeneric>([](HydraulicCurveGeneric& c) {
             c.points = {{-5, -1}, {0, 2}}; }), nullptr},
        {"pattern infinite", entity<HydraulicPatternTime>([](HydraulicPatternTime& p) {
             p.multipliers = {1, inf}; }), ".multipliers[1]"},
        {"material equal ages", entity<HydraulicPipeMaterial>([](HydraulicPipeMaterial& m) {
             m.roughness_by_age = {HydraulicPipeMaterialRoughnessAtAge{5, 130, {}, {}},
                                   HydraulicPipeMaterialRoughnessAtAge{5, 120, {}, {}}}; }), ".roughness_by_age[1]"},
        {"material roughness 0", entity<HydraulicPipeMaterial>([](HydraulicPipeMaterial& m) {
             m.roughness_by_age = {HydraulicPipeMaterialRoughnessAtAge{0, 0.0, {}, {}}}; }),
         ".roughness_by_age[0].roughness_hazen_williams"},
        {"material negative age", entity<HydraulicPipeMaterial>([](HydraulicPipeMaterial& m) {
             m.roughness_by_age = {HydraulicPipeMaterialRoughnessAtAge{-1, {}, {}, {}}}; }), ".roughness_by_age[0].age_years"},
        // Demand points, controls, labels
        {"demand point position 1.5", entity<HydraulicDemandPoint>([](HydraulicDemandPoint& d) {
             d.attachment.pipe_position = 1.5; }), ".attachment.pipe_position"},
        {"demand point position 1", entity<HydraulicDemandPoint>([](HydraulicDemandPoint& d) {
             d.attachment.pipe_position = 1; }), nullptr},
        {"simple control time of day 86400", entity<HydraulicControlSimple>([](HydraulicControlSimple& c) {
             c.trigger_time_of_day_s = 86400; }), ".trigger_time_of_day_s"},
        {"simple control time of day 86399", entity<HydraulicControlSimple>([](HydraulicControlSimple& c) {
             c.trigger_time_of_day_s = 86399; }), nullptr},
        {"simple control level negative", entity<HydraulicControlSimple>([](HydraulicControlSimple& c) {
             c.trigger_water_level_m = -1; }), ".trigger_water_level_m"},
        {"simple control valve position 101", entity<HydraulicControlSimple>([](HydraulicControlSimple& c) {
             c.setting.valve_position_percent = 101.0; }), ".setting.valve_position_percent"},
        {"rule premise time of day", entity<HydraulicControlRule>([](HydraulicControlRule& r) {
             r.premises.append(HydraulicControlRulePremise{}); r.premises[0].time_of_day_s = 90000; }),
         ".premises[0].time_of_day_s"},
        {"rule premise power negative", entity<HydraulicControlRule>([](HydraulicControlRule& r) {
             r.premises.append(HydraulicControlRulePremise{}); r.premises[0].power_kw = -1.0; }), ".premises[0].power_kw"},
        {"rule premise level negative", entity<HydraulicControlRule>([](HydraulicControlRule& r) {
             r.premises.append(HydraulicControlRulePremise{}); r.premises[0].water_level_m = -1.0; }),
         ".premises[0].water_level_m"},
        {"rule action speed negative", entity<HydraulicControlRule>([](HydraulicControlRule& r) {
             r.actions_else.append(HydraulicControlRuleAction{}); r.actions_else[0].setting.pump_speed_ratio = -1.0; }),
         ".actions_else[0].setting.pump_speed_ratio"},
        {"rule priority NaN", entity<HydraulicControlRule>([](HydraulicControlRule& r) { r.priority = nan; }), ".priority"},
        {"label longitude 200", entity<HydraulicMapLabel>([](HydraulicMapLabel& l) {
             l.coordinate_wgs84.longitude_deg = 200; }), ".coordinate_wgs84.longitude_deg"},
        // Network options
        {"hydraulic timestep 0", network([](NetworkHydraulic& n) { n.timestep_hydraulic_s = 0; }), ".timestep_hydraulic_s"},
        {"quality timestep 0", network([](NetworkHydraulic& n) { n.timestep_quality_s = 0; }), ".timestep_quality_s"},
        {"pattern timestep 0", network([](NetworkHydraulic& n) { n.timestep_pattern_s = 0; }), ".timestep_pattern_s"},
        {"report timestep 0", network([](NetworkHydraulic& n) { n.timestep_report_s = 0; }), ".timestep_report_s"},
        {"rule timestep 0", network([](NetworkHydraulic& n) { n.timestep_rule_s = 0; }), ".timestep_rule_s"},
        {"start time of day 86400", network([](NetworkHydraulic& n) { n.start_time_of_day_s = 86400; }), ".start_time_of_day_s"},
        {"accuracy 0", network([](NetworkHydraulic& n) { n.options_hydraulic.accuracy = 0; }), ".options_hydraulic.accuracy"},
        {"trials 0", network([](NetworkHydraulic& n) { n.options_hydraulic.maximum_trials = 0; }), ".options_hydraulic.maximum_trials"},
        {"check frequency 0", network([](NetworkHydraulic& n) { n.options_hydraulic.check_frequency = 0; }),
         ".options_hydraulic.check_frequency"},
        {"required below minimum pressure", network([](NetworkHydraulic& n) {
             n.options_hydraulic.minimum_pressure_head_m = 10; n.options_hydraulic.required_pressure_head_m = 5; }),
         ".options_hydraulic.required_pressure_head_m"},
        {"specific gravity 0", network([](NetworkHydraulic& n) { n.options_hydraulic.specific_gravity = 0; }),
         ".options_hydraulic.specific_gravity"},
        {"viscosity negative", network([](NetworkHydraulic& n) { n.options_hydraulic.relative_viscosity = -1; }),
         ".options_hydraulic.relative_viscosity"},
        {"demand multiplier negative", network([](NetworkHydraulic& n) { n.options_hydraulic.demand_multiplier = -1; }),
         ".options_hydraulic.demand_multiplier"},
        {"global efficiency 0", network([](NetworkHydraulic& n) { n.options_energy.global_pump_efficiency_percent = 0; }),
         ".options_energy.global_pump_efficiency_percent"},
        {"currency lowercase", network([](NetworkHydraulic& n) { n.options_energy.currency_iso4217 = QStringLiteral("eur"); }),
         ".options_energy.currency_iso4217"},
        {"currency EUR", network([](NetworkHydraulic& n) { n.options_energy.currency_iso4217 = QStringLiteral("EUR"); }), nullptr},
        {"global wall order 0.5", network([](NetworkHydraulic& n) { n.options_reaction.global_pipe_wall_reaction.order = 0.5; }),
         ".options_reaction.global_pipe_wall_reaction.order"},
        {"limiting concentration negative", network([](NetworkHydraulic& n) {
             n.options_reaction.limiting_concentration_mg_per_l = -1; }), ".options_reaction.limiting_concentration_mg_per_l"},
        {"report precision 16", network([](NetworkHydraulic& n) { n.options_report.fields_link.velocity.precision = 16; }),
         ".options_report.fields_link.velocity.precision"},
        {"report page size negative", network([](NetworkHydraulic& n) { n.options_report.page_size = -1; }),
         ".options_report.page_size"},
        {"report threshold NaN", network([](NetworkHydraulic& n) { n.options_report.fields_node.pressure.below_m = nan; }),
         ".options_report.fields_node.pressure.below_m"},
        {"msx timestep 0", network([](NetworkHydraulic& n) { n.multi_species.options.timestep_s = 0; }),
         ".multi_species.options.timestep_s"},
        {"msx segments 0", network([](NetworkHydraulic& n) { n.multi_species.options.maximum_segments = 0; }),
         ".multi_species.options.maximum_segments"},
        {"msx species tolerance negative", network([](NetworkHydraulic& n) {
             n.multi_species.species.append(MultiSpeciesSpecies{}); n.multi_species.species[0].absolute_tolerance = -1; }),
         ".multi_species.species[0].absolute_tolerance"},
        {"msx source negative", network([](NetworkHydraulic& n) {
             n.multi_species.sources.append(MultiSpeciesNodeSource{}); n.multi_species.sources[0].value = -1; }),
         ".multi_species.sources[0].value"},
        {"msx initial quality negative", network([](NetworkHydraulic& n) {
             n.multi_species.initial_quality_pipes.append(MultiSpeciesPipeInitialQuality{});
             n.multi_species.initial_quality_pipes[0].value = -1; }), ".multi_species.initial_quality_pipes[0].value"},
        {"msx pattern NaN", network([](NetworkHydraulic& n) {
             n.multi_species.patterns.append(MultiSpeciesPattern{}); n.multi_species.patterns[0].multipliers = {nan}; }),
         ".multi_species.patterns[0].multipliers[0]"},
        {"backdrop offset 400", network([](NetworkHydraulic& n) { n.map_backdrop.offset_longitude_deg = 400; }),
         ".map_backdrop.offset_longitude_deg"},
        {"entity inside network", network([](NetworkHydraulic& n) {
             n.links_pipes.append(HydraulicLinkPipe{}); n.links_pipes[0].diameter_mm = -1; }), ".diameter_mm"},
    };
    for (const Case& test : cases) {
        const ModelValidationReport report = test.run();
        if (test.path == nullptr) {
            expect(report.valid(), std::string(test.label) + ": valid value rejected (" +
                   report.summary().toStdString() + ")");
            continue;
        }
        expect(!report.valid(), std::string(test.label) + ": invalid value accepted");
        expect(report.issues.first().path.endsWith(QLatin1String(test.path)),
               std::string(test.label) + ": wrong issue " + report.summary().toStdString());
    }
    // Every issue is reported, not only the first.
    const ModelValidationReport several = entity<Pipe>([](Pipe& p) {
        p.diameter_mm = -1; p.minor_loss_coefficient = -1; p.leak_area_mm2_per_100m = -1; })();
    expect(several.issues.size() == 3, "not every issue reported");
    std::cout << "  " << cases.size() << " value rule cases\n";
}

std::filesystem::path temporaryDirectory() {
    std::string pattern = (std::filesystem::temp_directory_path() / "aowis-value-rules-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    char* directory = ::mkdtemp(name.data());
    expect(directory != nullptr, "mkdtemp failed");
    return directory;
}

void expectInvalidInput(const std::string& label, const std::function<void()>& action) {
    try {
        action();
    } catch (const native::StorageError& error) {
        expect(error.kind() == native::StorageErrorKind::InvalidInput,
               label + ": expected InvalidInput, got " + error.what());
        return;
    }
    throw std::runtime_error(label + ": accepted");
}

// Every save and edit path, including each indexed fast path, applies the
// value rules before anything is stored.
void testPersistencePaths() {
    const std::filesystem::path directory = temporaryDirectory();
    try {
        native::Repository repository(directory / "repo");
        const native::ProjectId project = native::ProjectId::generate();
        const native::RevisionId unbornHead = repository.createRootBranch(project, "main", commitMetadata());
        NetworkHydraulic network;
        network.uuid = QUuid::createUuid();
        HydraulicNodeReservoir reservoir; reservoir.uuid = QUuid::createUuid(); reservoir.hydraulic_head_m = 120;
        HydraulicNodeJunction first; first.uuid = QUuid::createUuid(); first.elevation_m = 50;
        HydraulicNodeJunction second; second.uuid = QUuid::createUuid(); second.elevation_m = 40;
        HydraulicNodeTank tank; tank.uuid = QUuid::createUuid(); tank.water_level_maximum_m = 5;
        HydraulicCurvePumpHead curve; curve.uuid = QUuid::createUuid(); curve.points = {{0, 60}, {50, 40}};
        HydraulicPatternTime pattern; pattern.uuid = QUuid::createUuid(); pattern.multipliers = {1.0, 1.2};
        HydraulicLinkPipe pipe; pipe.uuid = QUuid::createUuid();
        pipe.node_uuid_from = first.uuid; pipe.node_uuid_to = second.uuid; pipe.length_calculated_m = 100;
        HydraulicLinkPump pump; pump.uuid = QUuid::createUuid();
        pump.node_uuid_from = reservoir.uuid; pump.node_uuid_to = first.uuid; pump.head_curve_uuid = curve.uuid;
        HydraulicLinkValve valve; valve.uuid = QUuid::createUuid();
        valve.node_uuid_from = second.uuid; valve.node_uuid_to = tank.uuid; valve.diameter_mm = 150;
        HydraulicDemandPoint point; point.uuid = QUuid::createUuid();
        point.attachment.type = HydraulicDemandPointAttachmentType::Pipe;
        point.attachment.pipe_uuid = pipe.uuid; point.attachment.pipe_position = 0.5;
        network.nodes_reservoirs = {reservoir};
        network.nodes_junctions = {first, second};
        network.nodes_tanks = {tank};
        network.curves_pump_head = {curve};
        network.patterns_time = {pattern};
        network.links_pipes = {pipe};
        network.links_pumps = {pump};
        network.links_valves = {valve};
        network.demand_points = {point};

        NetworkHydraulic broken = network;
        broken.links_pipes[0].diameter_mm = -5;
        expectInvalidInput("save with negative diameter",
                           [&] { (void)hydraulic::saveHydraulicNetwork(repository, project, "main", broken, unbornHead, commitMetadata()); });
        broken = network;
        broken.options_hydraulic.accuracy = 0;
        expectInvalidInput("save with zero accuracy",
                           [&] { (void)hydraulic::saveHydraulicNetwork(repository, project, "main", broken, unbornHead, commitMetadata()); });
        const native::RevisionId saved = hydraulic::saveHydraulicNetwork(
            repository, project, "main", network, unbornHead, commitMetadata());

        const std::function<void(const std::string&, hydraulic::HydraulicEntity)> rejected =
            [&](const std::string& label, hydraulic::HydraulicEntity entity) {
                const QUuid id = std::visit([](const auto& value) { return value.uuid; }, entity);
                hydraulic::HydraulicEditDiagnostics diagnostics;
                expectInvalidInput(label, [&] {
                    (void)hydraulic::editHydraulicNetwork(repository, project, "main", network.uuid,
                        {{id, std::move(entity)}}, saved, commitMetadata(), &diagnostics);
                });
                expect(repository.head(project, "main") == saved,
                       label + ": repository changed");
            };
        // Physical fast path (valves, pumps, tanks, reservoirs).
        HydraulicLinkValve badValve = valve; badValve.diameter_mm = -1;
        rejected("physical edit: valve diameter", badValve);
        HydraulicLinkPump badPump = pump; badPump.constant_efficiency_percent = 150;
        rejected("physical edit: pump efficiency", badPump);
        HydraulicNodeTank badTank = tank; badTank.water_level_initial_m = 9;
        rejected("physical edit: tank levels", badTank);
        // Scalar fast path (pipes, junctions).
        HydraulicLinkPipe badPipe = pipe; badPipe.diameter_mm = 0;
        rejected("scalar edit: pipe diameter", badPipe);
        HydraulicNodeJunction badJunction = first; badJunction.elevation_m = nan;
        rejected("scalar edit: junction elevation NaN", badJunction);
        // Dependent fast path (demand points, controls).
        HydraulicDemandPoint badPoint = point; badPoint.attachment.pipe_position = 2;
        rejected("dependent edit: demand point position", badPoint);
        // Catalogue fast path (curves, patterns, materials).
        HydraulicCurvePumpHead badCurve = curve; badCurve.points = {{50, 40}, {0, 60}};
        rejected("catalogue edit: unordered curve", badCurve);
        HydraulicPatternTime badPattern = pattern; badPattern.multipliers = {inf};
        rejected("catalogue edit: infinite multiplier", badPattern);
        // Membership path (a new entity).
        HydraulicNodeJunction added; added.uuid = QUuid::createUuid(); added.emitter.coefficient = -1;
        rejected("membership edit: new junction", added);

        // Valid edits still take their fast paths.
        HydraulicLinkValve goodValve = valve; goodValve.diameter_mm = 200;
        hydraulic::HydraulicEditDiagnostics diagnostics;
        const native::RevisionId edited = hydraulic::editHydraulicNetwork(
            repository, project, "main", network.uuid,
            {{goodValve.uuid, hydraulic::HydraulicEntity{goodValve}}}, saved, commitMetadata(), &diagnostics);
        expect(edited != saved && diagnostics.indexedPhysicalFastPath &&
                   repository.head(project, "main") == edited,
               "valid physical edit not on its fast path");
        (void)repository.verifyRepository();
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        testGeneratedEnumTable();
        testCodecRejectsUndeclaredEnumerators();
        testCodecRejectsNonFiniteNumbers();
        testDraftDefaultsAreValid();
        testRuleTable();
        testPersistencePaths();
        std::cout << "Model value rule tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Model value rule test failed: " << error.what() << '\n';
        return 1;
    }
}
