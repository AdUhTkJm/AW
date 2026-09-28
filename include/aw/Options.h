#ifndef AW_OPTIONS_H
#define AW_OPTIONS_H

namespace aw {

// Process-wide planner options. Minecraft is serial and the crafting graph is
// a singleton, so there is no thread safety to worry about.
struct Options {
  bool nonoptimal = true;
};

// The singleton option block.
extern Options options;

}  // namespace aw

#endif
