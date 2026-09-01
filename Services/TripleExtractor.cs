using System.Text;

namespace VoiceTableAssist.Services;

/// <summary>从 RaNER 输出的 BIO 序列提取三元组 (SUB, OBJ, VAL)。</summary>
public static class TripleExtractor
{
    public static List<(string Sub, string Obj, string Val)> Extract(IEnumerable<(string Ch, string Tag)> bio)
    {
        var entities = new List<(string Text, string Type)>();
        string? curType = null;
        var curChars = new StringBuilder();
        foreach (var (ch, tag) in bio)
        {
            if (tag.StartsWith("B-"))
            {
                if (curType != null) entities.Add((curChars.ToString(), curType));
                curType = tag[2..]; curChars.Clear(); curChars.Append(ch);
            }
            else if (tag.StartsWith("I-"))
            {
                if (curType == tag[2..]) curChars.Append(ch);
                else { if (curType != null) entities.Add((curChars.ToString(), curType)); curType = null; curChars.Clear(); }
            }
            else
            {
                if (curType != null) { entities.Add((curChars.ToString(), curType)); curType = null; curChars.Clear(); }
            }
        }
        if (curType != null) entities.Add((curChars.ToString(), curType));

        var triples = new List<(string, string, string)>();
        int i = 0;
        string? curSub = null;
        while (i < entities.Count)
        {
            if (entities[i].Type == "SUB")
            {
                curSub = entities[i].Text; i++;
                while (i < entities.Count && entities[i].Type != "SUB")
                {
                    if (entities[i].Type == "OBJ" && i + 1 < entities.Count && entities[i + 1].Type == "VAL")
                    { triples.Add((curSub, entities[i].Text, entities[i + 1].Text)); i += 2; }
                    else if (entities[i].Type == "OBJ")
                    { triples.Add((curSub, entities[i].Text, "?")); i++; }
                    else if (entities[i].Type == "VAL")
                    { triples.Add((curSub, "?", entities[i].Text)); i++; }
                    else i++;
                }
            }
            else i++;
        }
        return triples;
    }
}