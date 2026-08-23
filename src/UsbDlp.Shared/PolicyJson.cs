using System.Text.Json;
using System.Text.Json.Serialization;
using UsbDlp.Contracts;

namespace UsbDlp.Policy;

public static class PolicyJson
{
    public static JsonSerializerOptions Options { get; } = CreateOptions();

    private static JsonSerializerOptions CreateOptions()
    {
        JsonSerializerOptions options = new(JsonSerializerDefaults.Web)
        {
            PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
            PropertyNameCaseInsensitive = false,
            ReadCommentHandling = JsonCommentHandling.Disallow,
            AllowTrailingCommas = false,
            UnmappedMemberHandling = JsonUnmappedMemberHandling.Skip,
            WriteIndented = true,
        };

        options.Converters.Add(new JsonStringEnumConverter<MatchType>(JsonNamingPolicy.SnakeCaseLower));
        options.Converters.Add(new JsonStringEnumConverter<EntryAction>(JsonNamingPolicy.SnakeCaseLower));
        options.Converters.Add(new JsonStringEnumConverter<GroupMode>(JsonNamingPolicy.SnakeCaseLower));
        options.Converters.Add(new JsonStringEnumConverter<ScopeType>(JsonNamingPolicy.SnakeCaseLower));
        options.Converters.Add(new JsonStringEnumConverter<Decision>(JsonNamingPolicy.SnakeCaseLower));
        options.Converters.Add(new JsonStringEnumConverter<ProductDeviceClass>(JsonNamingPolicy.SnakeCaseLower));
        options.Converters.Add(new JsonStringEnumConverter<AgentHealth>(JsonNamingPolicy.SnakeCaseLower));
        return options;
    }
}
