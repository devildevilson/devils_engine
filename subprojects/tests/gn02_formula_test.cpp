#include <array>
#include <cmath>
#include <fstream>
#include <iterator>
#include <numbers>
#include <string>
#include <vector>
#include <doctest/doctest.h>
#include "devils_engine/originator/script_program.h"

// Контракт переноса правил в данные: эталонные native формулы остаются только у теста.
// Проверяются реальные DS resources, полюса/нулевые оси, CSR ties и несколько переносов влаги.
using namespace devils_engine;
namespace o = devils_engine::originator;
namespace reference_gn02 { void add_planet_tools(o::tool_registry&); }
namespace {
using pair = std::pair<std::string_view, std::string_view>;
constexpr size_t count = 33;
o::field_ref read(const o::buffer& b, std::string_view field) { return {&b, nullptr, b.find_field(field)}; }
o::field_ref write(o::buffer& b, std::string_view field) { return {&b, &b, b.find_field(field)}; }
std::string source(std::string_view name) {
  std::ifstream input(std::string(GN02_FORMULA_ROOT) + "/" + std::string(name) + ".ds");
  REQUIRE(input.good());
  return {std::istreambuf_iterator<char>(input), {}};
}
std::unique_ptr<o::script_program> program(std::string_view name, std::span<const o::field_ref> inputs,
                                         const o::parameters& params, size_t own = SIZE_MAX) {
  std::vector<std::string> names, arguments;
  std::vector<o::field_type> types;
  for (size_t i = 0; i < inputs.size(); ++i) {
    names.push_back((i < own ? std::string{} : "other_") + std::string(inputs[i].field_name()));
    types.push_back(inputs[i].type());
  }
  for (size_t i = 0; i < params.size(); ++i) if (!params.is_string_at(i)) arguments.emplace_back(params.name_at(i));
  return o::script_program::compile(name, source(name), names, o::script_program::result_kind::number, types, arguments);
}
void map(std::string_view name, std::vector<o::field_ref> inputs, o::field_ref output, const o::parameters& params) {
  auto p = program(name, inputs, params);
  for (uint32_t c = 0; c < output.type().components; ++c)
    o::dispatch_script(*p, inputs, std::array{output}, params, 1, 0, count, "formula", nullptr, c);
}
void same(const o::buffer& a, const o::buffer& b, std::string_view field) {
  INFO("field=" << field);
  const auto x = a.field(a.find_field(field)), y = b.field(b.find_field(field));
  for (size_t i = 0; i < count; ++i) for (uint32_t c = 0; c < x.type().components; ++c) {
    INFO("element=" << i << " component=" << c);
    CHECK(x.get(i, c) == y.get(i, c));
  }
}
o::buffer cells() {
  const std::vector<pair> fields{{"position","v3"},{"plate_axis","v3"},{"plate_rate","v1"},
    {"velocity","v3"},{"wind","v3"},{"lat_sin","v1"},{"height","v1"},{"ocean_distance","v1"},
    {"land","ub1"},{"temperature","v1"},{"moisture","v1"},{"moisture_next","v1"},{"rain","v1"},
    {"crust","v1"},{"plate","ui1"},{"hotspot","v1"},{"convergence","v1"},{"shear","v1"},
    {"subduction","v1"},{"season","v1"}};
  o::buffer b("cells",o::make_buffer_layout(o::storage_kind::aos,fields,"cells"),count);
  for (size_t i=0;i<count;++i) {
    for (uint32_t c=0;c<3;++c) {
      b.field(b.find_field("position")).set(i,i==0?0:std::sin(double(i*7+c*11)),c);
      b.field(b.find_field("plate_axis")).set(i,i==1?0:std::cos(double(i*3+c*5)),c);
    }
    b.field(b.find_field("plate_rate")).set(i,double(i%5)*0.2);
    b.field(b.find_field("height")).set(i,double(i)*100-300);
    b.field(b.find_field("ocean_distance")).set(i,double(i)-2);
    b.field(b.find_field("land")).set(i,i%2);
    b.field(b.find_field("temperature")).set(i,double(i)-10);
    b.field(b.find_field("moisture")).set(i,double(i)*0.01);
    b.field(b.find_field("crust")).set(i,double(i%3)*0.25);
    b.field(b.find_field("plate")).set(i,i%4);
  }
  return b;
}
o::tool_registry reference() { o::tool_registry r; reference_gn02::add_planet_tools(r); return r; }
void native(o::tool_registry& r,std::string_view name,std::vector<o::field_ref> in,
            std::vector<o::field_ref> out,const o::parameters& p) {
  const auto* tool=r.find(name);
  REQUIRE(tool!=nullptr);
  o::dispatch(*tool,in,out,p,1,0,count,"reference",nullptr);
}
}

TEST_CASE("GN02 pointwise DS formulas match the original native rules") {
  auto ref=reference();
  for (int axis=0;axis<3;++axis) {
    auto old=cells(), made=old;
    o::parameters p; p.set_number("axis",axis);
    native(ref,"axis_component",{read(old,"position")},{write(old,"lat_sin")},p);
    map("axis",{read(made,"position")},write(made,"lat_sin"),p);
    same(old,made,"lat_sin");
    native(ref,"plate_velocity",{read(old,"position"),read(old,"plate_axis"),read(old,"plate_rate")},{write(old,"velocity")},{});
    map("velocity",{read(made,"position"),read(made,"plate_axis"),read(made,"plate_rate")},write(made,"velocity"),{});
    same(old,made,"velocity");
    p.set_number("bands",3); p.set_number("speed",1.25); p.set_number("meridional",0.4); p.set_number("rotation",1.2);
    native(ref,"wind_field",{read(old,"position"),read(old,"lat_sin")},{write(old,"wind")},p);
    map("wind",{read(made,"position"),read(made,"lat_sin")},write(made,"wind"),p);
    same(old,made,"wind");
    for (int season=-1;season<=1;++season) {
      o::parameters climate;
      climate.set_number("season",season); climate.set_number("tilt",23.44);
      climate.set_number("declination",23.44*std::numbers::pi/180.0*season);
      climate.set_number("equator_temperature",27); climate.set_number("pole_temperature",-26);
      climate.set_number("exponent",1.25); climate.set_number("lapse_rate",6.5);
      climate.set_number("continental_gain",0.95); climate.set_number("ocean_reference",8);
      native(ref,"insolation",{read(old,"lat_sin"),read(old,"height"),read(old,"ocean_distance")},{write(old,"season")},climate);
      map("insolation",{read(made,"lat_sin"),read(made,"height"),read(made,"ocean_distance")},write(made,"season"),climate);
      same(old,made,"season");
    }
  }
}

TEST_CASE("GN02 scripted neighbour reductions match native interactions and hotspots") {
  auto old=cells(), made=old;
  auto ref=reference();
  native(ref,"plate_velocity",{read(old,"position"),read(old,"plate_axis"),read(old,"plate_rate")},{write(old,"velocity")},{});
  made=old;
  const std::vector<pair> fields{{"offset","ui1"},{"arc","ui1"}};
  o::buffer csr("csr",o::make_buffer_layout(o::storage_kind::soa,fields,"csr"),count*2+1);
  for (size_t i=0;i<count;++i) {
    csr.field(0).set(i,i*2); csr.field(1).set(i*2,(i+1)%count); csr.field(1).set(i*2+1,(i+count-1)%count);
  }
  csr.field(0).set(count,count*2);
  native(ref,"plate_interaction",{read(csr,"offset"),read(csr,"arc"),read(old,"plate"),read(old,"velocity"),read(old,"position"),read(old,"crust")},
    {write(old,"convergence"),write(old,"shear"),write(old,"subduction")},{});
  const std::vector<o::field_ref> in{read(made,"plate"),read(made,"velocity"),read(made,"position"),read(made,"crust"),
    read(made,"plate"),read(made,"velocity"),read(made,"position"),read(made,"crust")};
  auto p=program("interaction",in,{},4);
  o::neighbourhood around=o::csr_neighbourhood{csr.field(0),csr.field(1),{},count};
  const std::vector<o::field_ref> out{write(made,"convergence"),write(made,"shear"),write(made,"subduction")};
  const std::array<o::script_reduction,3> rules{{{o::script_reduction::kind::maximum_absolute,0,-1},
    {o::script_reduction::kind::maximum,1,0},{o::script_reduction::kind::maximum_absolute,0,-1}}};
  const o::parameters empty;
  o::script_gather_call call;
  call.program=p.get();call.inputs=in;call.own_inputs=4;call.outputs=out;call.params=&empty;
  call.around=&around;call.other_count=count;call.reductions=rules;call.validity_component=3;call.end=count;call.step="interaction";
  o::dispatch_gather_program(call,nullptr);
  for (auto field:{"convergence","shear","subduction"}) same(old,made,field);
  const std::vector<pair> plume_fields{{"position","v3"},{"strength","v1"}};
  o::buffer plumes("plumes",o::make_buffer_layout(o::storage_kind::soa,plume_fields,"plumes"),3);
  for(size_t i=0;i<3;++i) {plumes.field(0).set(i,1.0,uint32_t(i));plumes.field(1).set(i,i==0?0.0:1.0);}
  o::parameters params;params.set_number("count",3);params.set_number("life",2);params.set_number("track_width",0.2);
  params.set_number("swell_width",0.08);params.set_number("swell_share",0.25);
  native(ref,"hotspot_tracks",{read(old,"position"),read(old,"plate_axis"),read(old,"plate_rate"),read(plumes,"position"),read(plumes,"strength")},{write(old,"hotspot")},params);
  const std::vector<o::field_ref> hot_in{read(made,"position"),read(made,"plate_axis"),read(made,"plate_rate"),read(plumes,"position"),read(plumes,"strength")};
  auto hot=program("hotspot",hot_in,params,3);
  const std::array<o::field_ref,1> hot_out{write(made,"hotspot")};
  const std::array<o::script_reduction,1> max{};
  call.program=hot.get();call.inputs=hot_in;call.own_inputs=3;call.outputs=hot_out;call.params=&params;
  call.around=nullptr;call.other_count=3;call.reductions=max;call.validity_component=-1;
  o::dispatch_gather_program(call,nullptr);
  same(old,made,"hotspot");
}

TEST_CASE("GN02 upwind selection and repeated moisture transfers match the original native rule") {
  auto old=cells();
  auto ref=reference();
  o::parameters wind;
  wind.set_number("axis",1);wind.set_number("bands",3);wind.set_number("speed",1);
  wind.set_number("meridional",0.4);wind.set_number("rotation",1);
  native(ref,"wind_field",{read(old,"position"),read(old,"lat_sin")},{write(old,"wind")},wind);
  auto made=old;
  const std::vector<pair> fields{{"offset","ui1"},{"arc","ui1"}};
  o::buffer csr("csr",o::make_buffer_layout(o::storage_kind::soa,fields,"csr"),count*2+1);
  for(size_t i=0;i<count;++i) {
    csr.field(0).set(i,i*2);csr.field(1).set(i*2,(i+1)%count);csr.field(1).set(i*2+1,(i+count-1)%count);
  }
  csr.field(0).set(count,count*2);
  const std::vector<pair> scratch_fields{{"upwind","ui1"},{"upwind_moisture","v1"},{"upwind_height","v1"}};
  o::buffer scratch("transport",o::make_buffer_layout(o::storage_kind::soa,scratch_fields,"transport"),count);
  const std::vector<o::field_ref> upstream{read(made,"position"),read(made,"wind"),read(made,"position")};
  auto upwind=program("upwind",upstream,{},2);
  const o::parameters empty;
  const std::array index_out{write(scratch,"upwind")};
  const std::array<o::script_reduction,1> min{{{o::script_reduction::kind::argmin,0,0}}};
  o::neighbourhood around=o::csr_neighbourhood{csr.field(0),csr.field(1),{},count};
  o::script_gather_call call;
  call.program=upwind.get();call.inputs=upstream;call.own_inputs=2;call.outputs=index_out;call.params=&empty;
  call.around=&around;call.other_count=count;call.reductions=min;call.empty_self=true;call.end=count;call.step="upwind";
  o::dispatch_gather_program(call,nullptr);
  o::parameters p;
  for(auto [name,value]:std::vector<std::pair<std::string_view,double>>{
    {"transport",0.94},{"evaporation",0.16},{"land_evaporation",0.03},{"capacity_base",0.35},
    {"capacity_gain",0.03},{"convective",0.06},{"convective_reference",26},{"convective_exponent",2},
    {"orographic",0.00055},{"ocean_rain",0.03}}) p.set_number(name,value);
  const std::vector<o::field_ref> in{read(scratch,"upwind_moisture"),read(scratch,"upwind_height"),
    read(made,"land"),read(made,"temperature"),read(made,"height"),read(made,"rain")};
  auto moisture=program("moisture",in,p);
  for(size_t iteration=0;iteration<8;++iteration) {
    const auto from=iteration%2==0?"moisture":"moisture_next";
    const auto to=iteration%2==0?"moisture_next":"moisture";
    native(ref,"moisture_step",{read(csr,"offset"),read(csr,"arc"),read(old,"position"),read(old,"wind"),
      read(old,from),read(old,"land"),read(old,"temperature"),read(old,"height")},
      {write(old,to),write(old,"rain")},p);
    for(size_t i=0;i<count;++i) {
      const auto other=size_t(scratch.field(0).get(i));
      scratch.field(1).set(i,made.field(made.find_field(from)).get(other));
      scratch.field(2).set(i,made.field(made.find_field("height")).get(other));
    }
    o::dispatch_script(*moisture,in,std::array{write(made,to)},p,1,0,count,"moisture",nullptr,0,0);
    o::dispatch_script(*moisture,in,std::array{write(made,"rain")},p,1,0,count,"rain",nullptr,0,1);
    same(old,made,to);same(old,made,"rain");
  }
}
