#pragma once
#include <format>
#include <string>
#include <string_view>
#include "devils_engine/utils/error.h"

// Один CSR shader kernel на понятие; старые graph_* и общий neighbourhood API
// отличаются только номерами привязок. Stencil пока CPU: отсутствие его формы
// объявляется отдельно, а не маскируется запуском CSR тела с недостающими входами.
namespace devils_engine::originator::detail {
inline std::string csr_device_body(std::string_view operation, unsigned values, unsigned offsets,
                                   unsigned arcs, int weights = -1, int mask = -1, bool arc_weights = false) {
  const auto read=[](unsigned input,std::string_view at) { return std::format("in_{}_at({})",input,at); };
  const auto own=read(values,"index"), other=read(values,"other");
  const auto loop=std::format("  uint first=uint({});\n  uint last=min(uint({}),in_{}_length());\n"
    "  for(uint k=first;k<last;++k) {{\n    uint other=uint({});\n    if(other>=in_{}_length()) continue;\n",
    read(offsets,"index"),read(offsets,"index+1u"),arcs,read(arcs,"k"),values);
  if(operation=="blur") {
    std::string body=std::format("  float sum=args.self_weight*{};\n  float weight=args.self_weight;\n",own)+loop;
    body+="    float w=args.neighbour_weight;\n";
    if(arc_weights) body+=std::format("#if ORIGINATOR_INPUTS > {}\n    w*=float({});\n#endif\n",weights,read(unsigned(weights),"k"));
    body+=std::format("    sum+=w*{}; weight+=w;\n  }}\n  out_0_set(index,weight>0.0?sum/weight:{});\n",other,own);
    return body;
  }
  if(operation=="frontier") return std::format("  float own={};\n  float edge=0.0;\n  if(own!=args.ignore) {{\n",own)+loop+
    std::format("    float label={};\n    if(label!=own && label!=args.ignore) {{ edge=1.0;break; }}\n  }}\n  }}\n  out_0_set(index,edge);\n",other);
  if(operation=="slope") return std::format("  float own={};\n  float total=0.0; uint taken=0u;\n",own)+loop+
    std::format("    total+=abs({}-own);++taken;\n  }}\n  out_0_set(index,taken==0u?0.0:total/float(taken));\n",other);
  if(operation=="vote") return std::format("  float own={};\n  if(own!=0.0 || {}==0.0) {{ out_0_set(index,own);return; }}\n"
    "  float label=0.0,weight=args.threshold;\n",own,read(unsigned(mask),"index"))+loop+
    std::format("    float candidate={};\n    if(candidate==0.0) continue;\n    float strength={};\n"
      "    if(strength>weight || (strength==weight && label!=0.0 && candidate<label)) {{ weight=strength;label=candidate; }}\n"
      "  }}\n  out_0_set(index,label);\n",other,read(unsigned(weights),"other"));
  utils::error{}("originator: unknown CSR shader operation '{}'",operation);
  return {};
}
}
