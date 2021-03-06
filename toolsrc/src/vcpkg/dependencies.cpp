#include "pch.h"

#include <vcpkg/base/files.h>
#include <vcpkg/base/graphs.h>
#include <vcpkg/base/strings.h>
#include <vcpkg/base/util.h>
#include <vcpkg/dependencies.h>
#include <vcpkg/packagespec.h>
#include <vcpkg/paragraphs.h>
#include <vcpkg/statusparagraphs.h>
#include <vcpkg/vcpkglib.h>
#include <vcpkg/vcpkgpaths.h>

namespace vcpkg::Dependencies
{
    struct FeatureNodeEdges
    {
        std::vector<FeatureSpec> remove_edges;
        std::vector<FeatureSpec> build_edges;
        bool plus = false;
    };

    struct Cluster : Util::MoveOnlyBase
    {
        std::vector<StatusParagraph*> status_paragraphs;
        Optional<const SourceControlFile*> source_control_file;
        PackageSpec spec;
        std::unordered_map<std::string, FeatureNodeEdges> edges;
        std::unordered_set<std::string> to_install_features;
        std::unordered_set<std::string> original_features;
        bool will_remove = false;
        bool transient_uninstalled = true;
        RequestType request_type = RequestType::AUTO_SELECTED;
    };

    struct ClusterPtr
    {
        Cluster* ptr;

        Cluster* operator->() const { return ptr; }
    };

    bool operator==(const ClusterPtr& l, const ClusterPtr& r) { return l.ptr == r.ptr; }
}

namespace std
{
    template<>
    struct hash<vcpkg::Dependencies::ClusterPtr>
    {
        size_t operator()(const vcpkg::Dependencies::ClusterPtr& value) const
        {
            return std::hash<vcpkg::PackageSpec>()(value.ptr->spec);
        }
    };
}

namespace vcpkg::Dependencies
{
    struct GraphPlan
    {
        Graphs::Graph<ClusterPtr> remove_graph;
        Graphs::Graph<ClusterPtr> install_graph;
    };

    struct ClusterGraph : Util::MoveOnlyBase
    {
        explicit ClusterGraph(const PortFileProvider& provider) : m_provider(provider) {}

        Cluster& get(const PackageSpec& spec)
        {
            auto it = m_graph.find(spec);
            if (it == m_graph.end())
            {
                // Load on-demand from m_provider
                auto maybe_scf = m_provider.get_control_file(spec.name());
                auto& clust = m_graph[spec];
                clust.spec = spec;
                if (auto p_scf = maybe_scf.get()) cluster_from_scf(*p_scf, clust);
                return clust;
            }
            return it->second;
        }

    private:
        void cluster_from_scf(const SourceControlFile& scf, Cluster& out_cluster) const
        {
            FeatureNodeEdges core_dependencies;
            core_dependencies.build_edges =
                filter_dependencies_to_specs(scf.core_paragraph->depends, out_cluster.spec.triplet());
            out_cluster.edges.emplace("core", std::move(core_dependencies));

            for (const auto& feature : scf.feature_paragraphs)
            {
                FeatureNodeEdges added_edges;
                added_edges.build_edges = filter_dependencies_to_specs(feature->depends, out_cluster.spec.triplet());
                out_cluster.edges.emplace(feature->name, std::move(added_edges));
            }
            out_cluster.source_control_file = &scf;
        }

        std::unordered_map<PackageSpec, Cluster> m_graph;
        const PortFileProvider& m_provider;
    };

    std::vector<PackageSpec> AnyParagraph::dependencies(const Triplet& triplet) const
    {
        if (const auto p = this->status_paragraph.get())
        {
            return PackageSpec::from_dependencies_of_port(p->package.spec.name(), p->package.depends, triplet);
        }

        if (const auto p = this->binary_control_file.get())
        {
            auto deps = Util::fmap_flatten(p->features, [](const BinaryParagraph& pgh) { return pgh.depends; });
            deps.insert(deps.end(), p->core_paragraph.depends.cbegin(), p->core_paragraph.depends.cend());
            return PackageSpec::from_dependencies_of_port(p->core_paragraph.spec.name(), deps, triplet);
        }

        if (const auto p = this->source_control_file.value_or(nullptr))
        {
            return PackageSpec::from_dependencies_of_port(
                p->core_paragraph->name, filter_dependencies(p->core_paragraph->depends, triplet), triplet);
        }

        Checks::exit_with_message(VCPKG_LINE_INFO,
                                  "Cannot get dependencies because there was none of: source/binary/status paragraphs");
    }

    std::string to_output_string(RequestType request_type,
                                 const CStringView s,
                                 const Build::BuildPackageOptions& options)
    {
        const char* const from_head = options.use_head_version == Build::UseHeadVersion::YES ? " (from HEAD)" : "";

        switch (request_type)
        {
            case RequestType::AUTO_SELECTED: return Strings::format("  * %s%s", s, from_head);
            case RequestType::USER_REQUESTED: return Strings::format("    %s%s", s, from_head);
            default: Checks::unreachable(VCPKG_LINE_INFO);
        }
    }

    std::string to_output_string(RequestType request_type, const CStringView s)
    {
        switch (request_type)
        {
            case RequestType::AUTO_SELECTED: return Strings::format("  * %s", s);
            case RequestType::USER_REQUESTED: return Strings::format("    %s", s);
            default: Checks::unreachable(VCPKG_LINE_INFO);
        }
    }

    InstallPlanAction::InstallPlanAction() : plan_type(InstallPlanType::UNKNOWN), request_type(RequestType::UNKNOWN) {}

    InstallPlanAction::InstallPlanAction(const PackageSpec& spec,
                                         const SourceControlFile& any_paragraph,
                                         const std::unordered_set<std::string>& features,
                                         const RequestType& request_type)
        : spec(spec), plan_type(InstallPlanType::BUILD_AND_INSTALL), request_type(request_type), feature_list(features)
    {
        this->any_paragraph.source_control_file = &any_paragraph;
    }

    InstallPlanAction::InstallPlanAction(const PackageSpec& spec,
                                         const std::unordered_set<std::string>& features,
                                         const RequestType& request_type)
        : spec(spec), plan_type(InstallPlanType::ALREADY_INSTALLED), request_type(request_type), feature_list(features)
    {
    }

    InstallPlanAction::InstallPlanAction(const PackageSpec& spec,
                                         const AnyParagraph& any_paragraph,
                                         const RequestType& request_type)
        : spec(spec), any_paragraph(any_paragraph), plan_type(InstallPlanType::UNKNOWN), request_type(request_type)
    {
        if (auto p = any_paragraph.status_paragraph.get())
        {
            this->plan_type = InstallPlanType::ALREADY_INSTALLED;
            return;
        }

        if (auto p = any_paragraph.binary_control_file.get())
        {
            this->plan_type = InstallPlanType::INSTALL;
            return;
        }

        if (auto p = any_paragraph.source_control_file.get())
        {
            this->plan_type = InstallPlanType::BUILD_AND_INSTALL;
            return;
        }

        Checks::unreachable(VCPKG_LINE_INFO);
    }

    std::string InstallPlanAction::displayname() const
    {
        if (this->feature_list.empty())
        {
            return this->spec.to_string();
        }

        const std::string features = Strings::join(",", this->feature_list);
        return Strings::format("%s[%s]:%s", this->spec.name(), features, this->spec.triplet());
    }

    bool InstallPlanAction::compare_by_name(const InstallPlanAction* left, const InstallPlanAction* right)
    {
        return left->spec.name() < right->spec.name();
    }

    RemovePlanAction::RemovePlanAction() : plan_type(RemovePlanType::UNKNOWN), request_type(RequestType::UNKNOWN) {}

    RemovePlanAction::RemovePlanAction(const PackageSpec& spec,
                                       const RemovePlanType& plan_type,
                                       const RequestType& request_type)
        : spec(spec), plan_type(plan_type), request_type(request_type)
    {
    }

    const PackageSpec& AnyAction::spec() const
    {
        if (const auto p = install_action.get())
        {
            return p->spec;
        }

        if (const auto p = remove_action.get())
        {
            return p->spec;
        }

        Checks::exit_with_message(VCPKG_LINE_INFO, "Null action");
    }

    bool ExportPlanAction::compare_by_name(const ExportPlanAction* left, const ExportPlanAction* right)
    {
        return left->spec.name() < right->spec.name();
    }

    ExportPlanAction::ExportPlanAction() : plan_type(ExportPlanType::UNKNOWN), request_type(RequestType::UNKNOWN) {}

    ExportPlanAction::ExportPlanAction(const PackageSpec& spec,
                                       const AnyParagraph& any_paragraph,
                                       const RequestType& request_type)
        : spec(spec), any_paragraph(any_paragraph), plan_type(ExportPlanType::UNKNOWN), request_type(request_type)
    {
        if (auto p = any_paragraph.binary_control_file.get())
        {
            this->plan_type = ExportPlanType::ALREADY_BUILT;
            return;
        }

        if (auto p = any_paragraph.source_control_file.get())
        {
            this->plan_type = ExportPlanType::PORT_AVAILABLE_BUT_NOT_BUILT;
            return;
        }
    }

    bool RemovePlanAction::compare_by_name(const RemovePlanAction* left, const RemovePlanAction* right)
    {
        return left->spec.name() < right->spec.name();
    }

    MapPortFileProvider::MapPortFileProvider(const std::unordered_map<std::string, SourceControlFile>& map) : ports(map)
    {
    }

    Optional<const SourceControlFile&> MapPortFileProvider::get_control_file(const std::string& spec) const
    {
        auto scf = ports.find(spec);
        if (scf == ports.end()) return nullopt;
        return scf->second;
    }

    PathsPortFileProvider::PathsPortFileProvider(const VcpkgPaths& paths) : ports(paths) {}

    Optional<const SourceControlFile&> PathsPortFileProvider::get_control_file(const std::string& spec) const
    {
        auto cache_it = cache.find(spec);
        if (cache_it != cache.end())
        {
            return cache_it->second;
        }
        Parse::ParseExpected<SourceControlFile> source_control_file =
            Paragraphs::try_load_port(ports.get_filesystem(), ports.port_dir(spec));

        if (auto scf = source_control_file.get())
        {
            auto it = cache.emplace(spec, std::move(*scf->get()));
            return it.first->second;
        }
        return nullopt;
    }

    std::vector<InstallPlanAction> create_install_plan(const PortFileProvider& port_file_provider,
                                                       const std::vector<PackageSpec>& specs,
                                                       const StatusParagraphs& status_db)
    {
        auto fspecs = Util::fmap(specs, [](const PackageSpec& spec) { return FeatureSpec(spec, ""); });
        auto plan = create_feature_install_plan(port_file_provider, fspecs, status_db);

        std::vector<InstallPlanAction> ret;
        ret.reserve(plan.size());

        for (auto&& action : plan)
        {
            if (auto p_install = action.install_action.get())
            {
                ret.push_back(std::move(*p_install));
            }
            else
            {
                Checks::exit_with_message(VCPKG_LINE_INFO,
                                          "The installation plan requires feature packages support. Please re-run the "
                                          "command with --featurepackages.");
            }
        }

        return ret;
    }

    std::vector<RemovePlanAction> create_remove_plan(const std::vector<PackageSpec>& specs,
                                                     const StatusParagraphs& status_db)
    {
        struct RemoveAdjacencyProvider final : Graphs::AdjacencyProvider<PackageSpec, RemovePlanAction>
        {
            const StatusParagraphs& status_db;
            const std::vector<StatusParagraph*>& installed_ports;
            const std::unordered_set<PackageSpec>& specs_as_set;

            RemoveAdjacencyProvider(const StatusParagraphs& status_db,
                                    const std::vector<StatusParagraph*>& installed_ports,
                                    const std::unordered_set<PackageSpec>& specs_as_set)
                : status_db(status_db), installed_ports(installed_ports), specs_as_set(specs_as_set)
            {
            }

            std::vector<PackageSpec> adjacency_list(const RemovePlanAction& plan) const override
            {
                if (plan.plan_type == RemovePlanType::NOT_INSTALLED)
                {
                    return {};
                }

                const PackageSpec& spec = plan.spec;
                std::vector<PackageSpec> dependents;
                for (const StatusParagraph* an_installed_package : installed_ports)
                {
                    if (an_installed_package->package.spec.triplet() != spec.triplet()) continue;

                    const std::vector<std::string>& deps = an_installed_package->package.depends;
                    if (std::find(deps.begin(), deps.end(), spec.name()) == deps.end()) continue;

                    dependents.push_back(an_installed_package->package.spec);
                }

                return dependents;
            }

            RemovePlanAction load_vertex_data(const PackageSpec& spec) const override
            {
                const RequestType request_type = specs_as_set.find(spec) != specs_as_set.end()
                                                     ? RequestType::USER_REQUESTED
                                                     : RequestType::AUTO_SELECTED;
                const StatusParagraphs::const_iterator it = status_db.find_installed(spec);
                if (it == status_db.end())
                {
                    return RemovePlanAction{spec, RemovePlanType::NOT_INSTALLED, request_type};
                }
                return RemovePlanAction{spec, RemovePlanType::REMOVE, request_type};
            }

            std::string to_string(const PackageSpec& spec) const override { return spec.to_string(); }
        };

        const std::vector<StatusParagraph*>& installed_ports = get_installed_ports(status_db);
        const std::unordered_set<PackageSpec> specs_as_set(specs.cbegin(), specs.cend());
        return Graphs::topological_sort(specs, RemoveAdjacencyProvider{status_db, installed_ports, specs_as_set});
    }

    std::vector<ExportPlanAction> create_export_plan(const PortFileProvider& port_file_provider,
                                                     const VcpkgPaths& paths,
                                                     const std::vector<PackageSpec>& specs,
                                                     const StatusParagraphs& status_db)
    {
        struct ExportAdjacencyProvider final : Graphs::AdjacencyProvider<PackageSpec, ExportPlanAction>
        {
            const VcpkgPaths& paths;
            const StatusParagraphs& status_db;
            const PortFileProvider& provider;
            const std::unordered_set<PackageSpec>& specs_as_set;

            ExportAdjacencyProvider(const VcpkgPaths& p,
                                    const StatusParagraphs& s,
                                    const PortFileProvider& prov,
                                    const std::unordered_set<PackageSpec>& specs_as_set)
                : paths(p), status_db(s), provider(prov), specs_as_set(specs_as_set)
            {
            }

            std::vector<PackageSpec> adjacency_list(const ExportPlanAction& plan) const override
            {
                return plan.any_paragraph.dependencies(plan.spec.triplet());
            }

            ExportPlanAction load_vertex_data(const PackageSpec& spec) const override
            {
                const RequestType request_type = specs_as_set.find(spec) != specs_as_set.end()
                                                     ? RequestType::USER_REQUESTED
                                                     : RequestType::AUTO_SELECTED;

                Expected<BinaryControlFile> maybe_bpgh = Paragraphs::try_load_cached_control_package(paths, spec);
                if (auto bcf = maybe_bpgh.get())
                    return ExportPlanAction{spec, AnyParagraph{nullopt, std::move(*bcf), nullopt}, request_type};

                auto maybe_scf = provider.get_control_file(spec.name());
                if (auto scf = maybe_scf.get()) return ExportPlanAction{spec, {nullopt, nullopt, scf}, request_type};

                Checks::exit_with_message(VCPKG_LINE_INFO, "Could not find package %s", spec);
            }

            std::string to_string(const PackageSpec& spec) const override { return spec.to_string(); }
        };

        const std::unordered_set<PackageSpec> specs_as_set(specs.cbegin(), specs.cend());
        std::vector<ExportPlanAction> toposort = Graphs::topological_sort(
            specs, ExportAdjacencyProvider{paths, status_db, port_file_provider, specs_as_set});
        return toposort;
    }

    enum class MarkPlusResult
    {
        FEATURE_NOT_FOUND,
        SUCCESS,
    };

    static MarkPlusResult mark_plus(const std::string& feature,
                                    Cluster& cluster,
                                    ClusterGraph& pkg_to_cluster,
                                    GraphPlan& graph_plan);

    static void mark_minus(Cluster& cluster, ClusterGraph& pkg_to_cluster, GraphPlan& graph_plan);

    MarkPlusResult mark_plus(const std::string& feature, Cluster& cluster, ClusterGraph& graph, GraphPlan& graph_plan)
    {
        if (feature.empty())
        {
            // Indicates that core was not specified in the reference
            return mark_plus("core", cluster, graph, graph_plan);
        }

        auto it = cluster.edges.find(feature);
        if (it == cluster.edges.end()) return MarkPlusResult::FEATURE_NOT_FOUND;

        if (cluster.edges[feature].plus) return MarkPlusResult::SUCCESS;

        if (cluster.original_features.find(feature) == cluster.original_features.end())
        {
            cluster.transient_uninstalled = true;
        }

        if (!cluster.transient_uninstalled)
        {
            return MarkPlusResult::SUCCESS;
        }
        cluster.edges[feature].plus = true;

        if (!cluster.original_features.empty())
        {
            mark_minus(cluster, graph, graph_plan);
        }

        graph_plan.install_graph.add_vertex({&cluster});
        auto& tracked = cluster.to_install_features;
        tracked.insert(feature);

        if (feature != "core")
        {
            // All features implicitly depend on core
            auto res = mark_plus("core", cluster, graph, graph_plan);

            // Should be impossible for "core" to not exist
            Checks::check_exit(VCPKG_LINE_INFO, res == MarkPlusResult::SUCCESS);
        }

        for (auto&& depend : cluster.edges[feature].build_edges)
        {
            auto& depend_cluster = graph.get(depend.spec());
            auto res = mark_plus(depend.feature(), depend_cluster, graph, graph_plan);

            Checks::check_exit(VCPKG_LINE_INFO,
                               res == MarkPlusResult::SUCCESS,
                               "Error: Unable to satisfy dependency %s of %s",
                               depend,
                               FeatureSpec(cluster.spec, feature));

            if (&depend_cluster == &cluster) continue;
            graph_plan.install_graph.add_edge({&cluster}, {&depend_cluster});
        }

        return MarkPlusResult::SUCCESS;
    }

    void mark_minus(Cluster& cluster, ClusterGraph& graph, GraphPlan& graph_plan)
    {
        if (cluster.will_remove) return;
        cluster.will_remove = true;

        graph_plan.remove_graph.add_vertex({&cluster});
        for (auto&& pair : cluster.edges)
        {
            auto& remove_edges_edges = pair.second.remove_edges;
            for (auto&& depend : remove_edges_edges)
            {
                auto& depend_cluster = graph.get(depend.spec());
                graph_plan.remove_graph.add_edge({&cluster}, {&depend_cluster});
                mark_minus(depend_cluster, graph, graph_plan);
            }
        }

        cluster.transient_uninstalled = true;
        for (auto&& original_feature : cluster.original_features)
        {
            auto res = mark_plus(original_feature, cluster, graph, graph_plan);
            if (res != MarkPlusResult::SUCCESS)
            {
                System::println(System::Color::warning,
                                "Warning: could not reinstall feature %s",
                                FeatureSpec{cluster.spec, original_feature});
            }
        }
    }

    std::vector<AnyAction> create_feature_install_plan(const PortFileProvider& provider,
                                                       const std::vector<FeatureSpec>& specs,
                                                       const StatusParagraphs& status_db)
    {
        PackageGraph pgraph(provider, status_db);
        for (auto&& spec : specs)
            pgraph.install(spec);

        return pgraph.serialize();
    }

    std::vector<AnyAction> create_feature_install_plan(const std::unordered_map<std::string, SourceControlFile>& map,
                                                       const std::vector<FeatureSpec>& specs,
                                                       const StatusParagraphs& status_db)
    {
        MapPortFileProvider provider(map);
        return create_feature_install_plan(provider, specs, status_db);
    }

    void PackageGraph::install(const FeatureSpec& spec)
    {
        Cluster& spec_cluster = m_graph->get(spec.spec());
        spec_cluster.request_type = RequestType::USER_REQUESTED;
        if (spec.feature() == "*")
        {
            if (auto p_scf = spec_cluster.source_control_file.value_or(nullptr))
            {
                for (auto&& feature : p_scf->feature_paragraphs)
                {
                    auto res = mark_plus(feature->name, spec_cluster, *m_graph, *m_graph_plan);

                    Checks::check_exit(
                        VCPKG_LINE_INFO, res == MarkPlusResult::SUCCESS, "Error: Unable to locate feature %s", spec);
                }

                auto res = mark_plus("core", spec_cluster, *m_graph, *m_graph_plan);

                Checks::check_exit(
                    VCPKG_LINE_INFO, res == MarkPlusResult::SUCCESS, "Error: Unable to locate feature %s", spec);
            }
            else
            {
                Checks::exit_with_message(
                    VCPKG_LINE_INFO, "Error: Unable to handle '*' because can't find CONTROL for %s", spec.spec());
            }
        }
        else
        {
            auto res = mark_plus(spec.feature(), spec_cluster, *m_graph, *m_graph_plan);

            Checks::check_exit(
                VCPKG_LINE_INFO, res == MarkPlusResult::SUCCESS, "Error: Unable to locate feature %s", spec);
        }

        m_graph_plan->install_graph.add_vertex(ClusterPtr{&spec_cluster});
    }

    void PackageGraph::upgrade(const PackageSpec& spec)
    {
        Cluster& spec_cluster = m_graph->get(spec);
        spec_cluster.request_type = RequestType::USER_REQUESTED;

        mark_minus(spec_cluster, *m_graph, *m_graph_plan);
    }

    std::vector<AnyAction> PackageGraph::serialize() const
    {
        auto remove_vertex_list = m_graph_plan->remove_graph.vertex_list();
        auto remove_toposort = Graphs::topological_sort(remove_vertex_list, m_graph_plan->remove_graph);

        auto insert_vertex_list = m_graph_plan->install_graph.vertex_list();
        auto insert_toposort = Graphs::topological_sort(insert_vertex_list, m_graph_plan->install_graph);

        std::vector<AnyAction> plan;

        for (auto&& p_cluster : remove_toposort)
        {
            auto scf = *p_cluster->source_control_file.get();
            auto spec = PackageSpec::from_name_and_triplet(scf->core_paragraph->name, p_cluster->spec.triplet())
                            .value_or_exit(VCPKG_LINE_INFO);
            plan.emplace_back(RemovePlanAction{
                std::move(spec),
                RemovePlanType::REMOVE,
                p_cluster->request_type,
            });
        }

        for (auto&& p_cluster : insert_toposort)
        {
            if (p_cluster->transient_uninstalled)
            {
                // If it will be transiently uninstalled, we need to issue a full installation command
                auto pscf = p_cluster->source_control_file.value_or_exit(VCPKG_LINE_INFO);
                Checks::check_exit(VCPKG_LINE_INFO, pscf != nullptr);
                plan.emplace_back(InstallPlanAction{
                    p_cluster->spec,
                    *pscf,
                    p_cluster->to_install_features,
                    p_cluster->request_type,
                });
            }
            else
            {
                // If the package isn't transitively installed, still include it if the user explicitly requested it
                if (p_cluster->request_type != RequestType::USER_REQUESTED) continue;
                plan.emplace_back(InstallPlanAction{
                    p_cluster->spec,
                    p_cluster->original_features,
                    p_cluster->request_type,
                });
            }
        }

        return plan;
    }

    static std::unique_ptr<ClusterGraph> create_feature_install_graph(const PortFileProvider& map,
                                                                      const StatusParagraphs& status_db)
    {
        std::unique_ptr<ClusterGraph> graph = std::make_unique<ClusterGraph>(map);

        auto installed_ports = get_installed_ports(status_db);

        for (auto&& status_paragraph : installed_ports)
        {
            Cluster& cluster = graph->get(status_paragraph->package.spec);

            cluster.transient_uninstalled = false;

            cluster.status_paragraphs.emplace_back(status_paragraph);

            auto& status_paragraph_feature = status_paragraph->package.feature;
            // In this case, empty string indicates the "core" paragraph for a package.
            if (status_paragraph_feature.empty())
            {
                cluster.original_features.insert("core");
            }
            else
            {
                cluster.original_features.insert(status_paragraph_feature);
            }
        }

        // Populate the graph with "remove edges", which are the reverse of the Build-Depends edges.
        for (auto&& status_paragraph : installed_ports)
        {
            auto& spec = status_paragraph->package.spec;
            auto& status_paragraph_feature = status_paragraph->package.feature;
            auto reverse_edges = FeatureSpec::from_strings_and_triplet(status_paragraph->package.depends,
                                                                       status_paragraph->package.spec.triplet());

            for (auto&& dependency : reverse_edges)
            {
                auto& dep_cluster = graph->get(dependency.spec());

                auto depends_name = dependency.feature();
                if (depends_name.empty()) depends_name = "core";

                auto& target_node = dep_cluster.edges[depends_name];
                target_node.remove_edges.emplace_back(FeatureSpec{spec, status_paragraph_feature});
            }
        }
        return graph;
    }

    PackageGraph::PackageGraph(const PortFileProvider& provider, const StatusParagraphs& status_db)
        : m_graph(create_feature_install_graph(provider, status_db)), m_graph_plan(std::make_unique<GraphPlan>())
    {
    }

    PackageGraph::~PackageGraph() {}

    void print_plan(const std::vector<AnyAction>& action_plan, const bool is_recursive)
    {
        std::vector<const RemovePlanAction*> remove_plans;
        std::vector<const InstallPlanAction*> rebuilt_plans;
        std::vector<const InstallPlanAction*> only_install_plans;
        std::vector<const InstallPlanAction*> new_plans;
        std::vector<const InstallPlanAction*> already_installed_plans;
        std::vector<const InstallPlanAction*> excluded;

        const bool has_non_user_requested_packages = Util::find_if(action_plan, [](const AnyAction& package) -> bool {
                                                         if (auto iplan = package.install_action.get())
                                                             return iplan->request_type != RequestType::USER_REQUESTED;
                                                         else
                                                             return false;
                                                     }) != action_plan.cend();

        for (auto&& action : action_plan)
        {
            if (auto install_action = action.install_action.get())
            {
                // remove plans are guaranteed to come before install plans, so we know the plan will be contained if at
                // all.
                auto it = Util::find_if(
                    remove_plans, [&](const RemovePlanAction* plan) { return plan->spec == install_action->spec; });
                if (it != remove_plans.end())
                {
                    rebuilt_plans.emplace_back(install_action);
                }
                else
                {
                    switch (install_action->plan_type)
                    {
                        case InstallPlanType::INSTALL: only_install_plans.emplace_back(install_action); break;
                        case InstallPlanType::ALREADY_INSTALLED:
                            if (install_action->request_type == RequestType::USER_REQUESTED)
                                already_installed_plans.emplace_back(install_action);
                            break;
                        case InstallPlanType::BUILD_AND_INSTALL: new_plans.emplace_back(install_action); break;
                        case InstallPlanType::EXCLUDED: excluded.emplace_back(install_action); break;
                        default: Checks::unreachable(VCPKG_LINE_INFO);
                    }
                }
            }
            else if (auto remove_action = action.remove_action.get())
            {
                remove_plans.emplace_back(remove_action);
            }
        }

        std::sort(remove_plans.begin(), remove_plans.end(), &RemovePlanAction::compare_by_name);
        std::sort(rebuilt_plans.begin(), rebuilt_plans.end(), &InstallPlanAction::compare_by_name);
        std::sort(only_install_plans.begin(), only_install_plans.end(), &InstallPlanAction::compare_by_name);
        std::sort(new_plans.begin(), new_plans.end(), &InstallPlanAction::compare_by_name);
        std::sort(already_installed_plans.begin(), already_installed_plans.end(), &InstallPlanAction::compare_by_name);
        std::sort(excluded.begin(), excluded.end(), &InstallPlanAction::compare_by_name);

        static auto actions_to_output_string = [](const std::vector<const InstallPlanAction*>& v) {
            return Strings::join("\n", v, [](const InstallPlanAction* p) {
                return to_output_string(p->request_type, p->displayname(), p->build_options);
            });
        };

        if (excluded.size() > 0)
        {
            System::println("The following packages are excluded:\n%s", actions_to_output_string(excluded));
        }

        if (already_installed_plans.size() > 0)
        {
            System::println("The following packages are already installed:\n%s",
                            actions_to_output_string(already_installed_plans));
        }

        if (rebuilt_plans.size() > 0)
        {
            System::println("The following packages will be rebuilt:\n%s", actions_to_output_string(rebuilt_plans));
        }

        if (new_plans.size() > 0)
        {
            System::println("The following packages will be built and installed:\n%s",
                            actions_to_output_string(new_plans));
        }

        if (only_install_plans.size() > 0)
        {
            System::println("The following packages will be directly installed:\n%s",
                            actions_to_output_string(only_install_plans));
        }

        if (has_non_user_requested_packages)
            System::println("Additional packages (*) will be modified to complete this operation.");

        if (remove_plans.size() > 0 && !is_recursive)
        {
            System::println(System::Color::warning,
                            "If you are sure you want to rebuild the above packages, run the command with the "
                            "--recurse option");
            Checks::exit_fail(VCPKG_LINE_INFO);
        }
    }
}
