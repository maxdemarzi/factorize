#include "materialize.hpp"

#include <algorithm>
#include <cassert>
#include <functional>
#include <stdexcept>

namespace factorize {

TreeTopology TreeTopology::Of(const FTree &tree) {
	TreeTopology topology;
	std::function<void(const FNode &, int32_t, int32_t)> visit = [&](const FNode &node, int32_t parent,
	                                                                int32_t slot) {
		const auto self = static_cast<int32_t>(topology.parent.size());
		topology.parent.push_back(parent);
		topology.slot_in_parent.push_back(slot);
		for (size_t i = 0; i < node.Children().size(); i++) {
			visit(*node.Children()[i], self, static_cast<int32_t>(i));
		}
	};
	visit(tree.Root(), -1, -1);
	return topology;
}

MaterializePlan MaterializePlan::Build(const FTree &input_tree, const Layout &input_layout, const FTree &output_tree,
                                       const Layout &output_layout,
                                       const std::vector<std::vector<uint32_t>> &sources,
                                       const std::vector<bool> &owned, uint32_t root_output_level) {
	const auto input_topology = TreeTopology::Of(input_tree);
	const auto output_topology = TreeTopology::Of(output_tree);

	MaterializePlan plan;

	// Walk the owned output levels top-down so a parent always gets a lower
	// plan index than its children; IterateLevel relies on that ordering.
	std::function<void(uint32_t)> collect = [&](uint32_t output_level) {
		plan.output_to_plan[output_level] = static_cast<uint32_t>(plan.levels.size());
		PlanLevel level;
		level.output_level = output_level;
		plan.levels.push_back(std::move(level));
		// Children of this output level, in slot order.
		for (uint32_t candidate = 0; candidate < output_topology.size(); candidate++) {
			if (output_topology.parent[candidate] != static_cast<int32_t>(output_level)) {
				continue;
			}
			if (!owned[candidate]) {
				continue;
			}
			collect(candidate);
		}
	};
	collect(root_output_level);

	// Which owned output level each input node ended up in.
	std::unordered_map<uint32_t, uint32_t> input_to_output;
	for (const auto &level : plan.levels) {
		for (auto input_level : sources[level.output_level]) {
			input_to_output[input_level] = level.output_level;
		}
	}

	for (size_t plan_index = 0; plan_index < plan.levels.size(); plan_index++) {
		auto &level = plan.levels[plan_index];
		const auto &level_sources = sources[level.output_level];

		// Order the sources so that any whose parent lives in this same level
		// follows it: the nested loop must fix the parent before the child.
		std::vector<uint32_t> ordered;
		std::vector<bool> placed(level_sources.size(), false);
		bool progress = true;
		while (ordered.size() < level_sources.size() && progress) {
			progress = false;
			for (size_t i = 0; i < level_sources.size(); i++) {
				if (placed[i]) {
					continue;
				}
				const auto input_level = level_sources[i];
				const auto parent = input_topology.parent[input_level];
				bool ready = true;
				if (parent >= 0) {
					auto entry = input_to_output.find(static_cast<uint32_t>(parent));
					if (entry == input_to_output.end()) {
						throw std::runtime_error("materialize: input node's parent is not owned by any output level");
					}
					if (entry->second == level.output_level) {
						// Sibling dependency: the parent must already be placed.
						ready = std::find(ordered.begin(), ordered.end(), static_cast<uint32_t>(parent)) !=
						        ordered.end();
					}
				}
				if (ready) {
					ordered.push_back(input_level);
					placed[i] = true;
					progress = true;
				}
			}
		}
		if (ordered.size() != level_sources.size()) {
			throw std::runtime_error("materialize: cyclic source dependency within an output level");
		}

		for (auto input_level : ordered) {
			SourceRef ref {};
			ref.input_level = input_level;
			const auto parent = input_topology.parent[input_level];
			if (parent < 0) {
				ref.parent_plan_index = -1;
				ref.parent_source_index = -1;
				ref.slot = -1;
			} else {
				const auto parent_output = input_to_output.at(static_cast<uint32_t>(parent));
				ref.parent_plan_index = static_cast<int32_t>(plan.output_to_plan.at(parent_output));
				const auto &parent_sources = sources[parent_output];
				// The index within the parent level's *ordered* source list.
				const auto &parent_level = plan.levels[static_cast<size_t>(ref.parent_plan_index)];
				int32_t position = -1;
				if (parent_output == level.output_level) {
					for (size_t i = 0; i < ordered.size(); i++) {
						if (ordered[i] == static_cast<uint32_t>(parent)) {
							position = static_cast<int32_t>(i);
						}
					}
				} else {
					for (size_t i = 0; i < parent_level.sources.size(); i++) {
						if (parent_level.sources[i].input_level == static_cast<uint32_t>(parent)) {
							position = static_cast<int32_t>(i);
						}
					}
				}
				if (position < 0) {
					(void)parent_sources;
					throw std::runtime_error("materialize: parent source not found");
				}
				ref.parent_source_index = position;
				ref.slot = input_topology.slot_in_parent[input_level];
			}
			level.sources.push_back(ref);
		}

		// Payload: for every attribute of the output level, find the input
		// record holding it.
		const auto &out_level_layout = output_layout.Level(static_cast<LevelId>(level.output_level));
		for (size_t out_index = 0; out_index < out_level_layout.payload.size(); out_index++) {
			const auto attribute = out_level_layout.payload[out_index].attribute;
			bool found = false;
			for (size_t source_index = 0; source_index < level.sources.size() && !found; source_index++) {
				const auto input_level = level.sources[source_index].input_level;
				const auto &in_level_layout = input_layout.Level(static_cast<LevelId>(input_level));
				for (size_t in_index = 0; in_index < in_level_layout.payload.size(); in_index++) {
					if (in_level_layout.payload[in_index].attribute != attribute) {
						continue;
					}
					PayloadCopy copy {};
					copy.source_index = static_cast<uint32_t>(source_index);
					copy.input_payload = static_cast<uint32_t>(in_index);
					copy.output_payload = static_cast<uint32_t>(out_index);
					copy.type = in_level_layout.payload[in_index].type;
					if (copy.type != out_level_layout.payload[out_index].type) {
						throw std::runtime_error("materialize: attribute changed type across the join");
					}
					level.payload.push_back(copy);
					found = true;
					break;
				}
			}
			if (!found) {
				throw std::runtime_error("materialize: no input holds output attribute " +
				                         std::to_string(attribute));
			}
		}
	}

	// Link each plan level to the owned output children, recording the slot the
	// child occupies in the output record.
	for (size_t plan_index = 0; plan_index < plan.levels.size(); plan_index++) {
		const auto output_level = plan.levels[plan_index].output_level;
		const auto &out_level_layout = output_layout.Level(static_cast<LevelId>(output_level));
		for (size_t slot = 0; slot < out_level_layout.slots.size(); slot++) {
			const auto child_output_level = out_level_layout.slots[slot].child_level;
			auto entry = plan.output_to_plan.find(child_output_level);
			if (entry == plan.output_to_plan.end()) {
				// Not owned: this is the seam where the other side attaches.
				continue;
			}
			plan.levels[plan_index].children.emplace_back(entry->second, static_cast<uint32_t>(slot));
		}
	}

	// Lay out the flat context array.
	uint32_t offset = 0;
	for (auto &level : plan.levels) {
		level.ctx_offset = offset;
		offset += static_cast<uint32_t>(level.sources.size());
	}
	plan.ctx_size = offset;
	return plan;
}

std::string MaterializePlan::ToString() const {
	std::string out;
	for (size_t i = 0; i < levels.size(); i++) {
		const auto &level = levels[i];
		out += "  plan[" + std::to_string(i) + "] -> out L" + std::to_string(level.output_level) + " sources=[";
		for (size_t j = 0; j < level.sources.size(); j++) {
			if (j > 0) {
				out += ",";
			}
			const auto &ref = level.sources[j];
			out += "in" + std::to_string(ref.input_level);
			if (ref.parent_plan_index >= 0) {
				out += "(<-plan" + std::to_string(ref.parent_plan_index) + "." +
				       std::to_string(ref.parent_source_index) + " slot" + std::to_string(ref.slot) + ")";
			} else {
				out += "(root)";
			}
		}
		out += "]";
		if (!level.children.empty()) {
			out += " children=[";
			for (size_t j = 0; j < level.children.size(); j++) {
				if (j > 0) {
					out += ",";
				}
				out += "plan" + std::to_string(level.children[j].first) + "@slot" +
				       std::to_string(level.children[j].second);
			}
			out += "]";
		}
		out += "\n";
	}
	return out;
}

} // namespace factorize
