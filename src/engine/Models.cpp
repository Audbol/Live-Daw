#include "engine/Models.h"

namespace livedaw::models
{

juce::String Param::display (float value) const
{
    return takeString (ld::ld_model_param_display (table, model, index, value));
}

float Param::toNormalised (float value) const
{
    return ld::ld_model_param_to_normalised (table, model, index, value);
}

float Param::fromNormalised (float position) const
{
    return ld::ld_model_param_from_normalised (table, model, index, position);
}

bool dynStaticCurve (int model, const float* values, int numValues, const float* inDb, float* outDb, int count)
{
    return ld::ld_dyn_static_curve (model, values, numValues, inDb, outDb, count);
}

int Model::find (const juce::String& id) const noexcept
{
    for (size_t i = 0; i < params.size(); ++i)
        if (params[i].id == id)
            return (int) i;
    return -1;
}

static std::vector<Model> build (Table t)
{
    std::vector<Model> models;
    const int n = ld::ld_model_count (t);
    models.reserve ((size_t) n);

    for (int m = 0; m < n; ++m)
    {
        Model model;
        model.table = t;
        model.index = m;
        model.token = takeString (ld::ld_model_text (t, m, 0));
        model.name = takeString (ld::ld_model_text (t, m, 1));
        model.category = static_cast<Category> (juce::jlimit (0, 9, (int) ld::ld_model_category (t, m)));

        const int np = ld::ld_model_num_params (t, m);
        for (int p = 0; p < np; ++p)
        {
            ld::ParamInfo info {};
            ld::ld_model_param_info (t, m, p, &info);

            Param param;
            param.table = t;
            param.model = m;
            param.index = p;
            param.id = takeString (ld::ld_model_param_text (t, m, p, 0));
            param.label = takeString (ld::ld_model_param_text (t, m, p, 1));
            param.unit = takeString (ld::ld_model_param_text (t, m, p, 2));
            param.kind = static_cast<ParamKind> (juce::jlimit (0, 5, (int) info.kind));
            param.min = info.min;
            param.max = info.max;
            param.steps = info.steps;
            param.defaultValue = info.defaultValue;
            for (int c = 0; c < info.numChoices; ++c)
                param.choices.add (takeString (ld::ld_model_param_choice (t, m, p, c)));
            model.params.push_back (std::move (param));
        }

        models.push_back (std::move (model));
    }

    return models;
}

const std::vector<Model>& table (Table t)
{
    static const std::vector<Model> tables[] = { build (Dynamics), build (ChannelEq), build (BusEq), build (FilterTools), build (Fx) };
    return tables[juce::jlimit (0, 4, (int) t)];
}

const Model& model (Table t, int index)
{
    const auto& models = table (t);
    jassert (! models.empty());
    return models[(size_t) juce::jlimit (0, (int) models.size() - 1, index)];
}

int findModel (Table t, const juce::String& token)
{
    const auto& models = table (t);
    for (size_t i = 0; i < models.size(); ++i)
        if (models[i].token == token)
            return (int) i;
    return -1;
}

const std::vector<FxMenuTab>& fxMenuTabs()
{
    static const std::vector<FxMenuTab> tabs = []
    {
        using Rows = std::vector<std::vector<const char*>>;
        auto tab = [] (const char* title, const char* subtitle, bool premium, const Rows& rows)
        {
            FxMenuTab t { title, subtitle, premium, {} };
            for (const auto& row : rows)
            {
                auto& r = t.rows.emplace_back();
                for (const auto* token : row)
                    if (const int i = findModel (Fx, token); i >= 0)
                        r.push_back (i);
            }
            return t;
        };
        // The console's order: reverbs, the vintage units, special reverbs,
        // modulation, delays and pitch; then the standard units by family.
        return std::vector<FxMenuTab> {
            tab ("FX1-8", "", true,
                 { { "HALL", "ROOM", "CHAMBER", "PLATE", "CONCERT", "AMBI" },
                   { "VSS3", "V-ROOM", "V-REV", "V-PLATE", "BPLATE" },
                   { "GATED", "REVERSE", "DEL/REV", "SHIMMER", "SPRING" },
                   { "DIMCRS", "CHORUS", "FLANGER" },
                   { "ST-DL", "TAP-DL", "TAPE-DL", "OILCAN", "BBD-DL" },
                   { "PITCH", "D-PITCH" } }),
            tab ("STD", "ALL FX", false,
                 { { "GEQ", "PIA", "SPKMAN" },
                   { "DEQ3", "C5-CMB", "LIMITER" },
                   { "DE-S2", "ENHANCE", "EXCITER", "P-BASS", "SUB", "SUB-M" },
                   { "V-IMG", "DOUBLE", "PCORR" },
                   { "ROTARY", "PHASER", "PANNER", "TAPE", "MOOD" },
                   { "BODY", "RACKAMP", "UKROCK", "ANGEL", "JAZZC", "DELUXE" } }),
            tab ("CH", "ALL FX", false,
                 { { "SOUL", "E88", "E84", "F110", "PULSAR", "MACH4" },
                   { "*EVEN*", "*SOUL*", "*VINTAGE*", "*BUS*", "*MASTER*" } }),
        };
    }();
    return tabs;
}

juce::StringArray list (List l)
{
    juce::StringArray items;
    const int n = ld::ld_list_count (l);
    for (int i = 0; i < n; ++i)
        items.add (takeString (ld::ld_list_item (l, i)));
    return items;
}

} // namespace livedaw::models
