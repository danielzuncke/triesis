import std;

int main(string[] args)
{
	// Fail compilation for operating systems other than linux:
	// dfmt off
	version(linux) {}
	else { static assert(0, "not implemented for selected operating system"); }
	// dfmt on

	return run(args);
}

int run(string[] args)
{
	string pathDataset;
	string outputName = "dataset";
	Unicode encoding;
	bool forceBigEndian;
	// size_t limit = size_t.max;
	// dfmt off
	auto helpInfo = getopt(
		args,
		std.getopt.config.required,
		"file|f", "input dataset file; must be valid unicode, '\\n' seperated", &pathDataset,
		"output|o", "output name of the string-file, no extension; will create: <name>{,_meta}.bin", &outputName,
		std.getopt.config.required,
		"encoding|e", "Unicode encoding (utf8, utf16, utf16sort, utf32)",
			(string _, string value) {
				switch (value.toLower)
				{
					case "utf8": encoding = Unicode.Utf_8; break;
					case "utf16": encoding = Unicode.Utf_16; break;
					case "utf16sort": encoding = Unicode.Utf_16_lex; break;
					case "utf32": encoding = Unicode.Utf_32; break;
					default: throw new Exception("invalid encoding: " ~ value);
				}
			},
		"be", "write the underlying type as big endian to memory, so it is bytewise comparable",
			&forceBigEndian,
		// "l|limit", "remove lines longer than given value while loading", &limit,
		);
	// dfmt on
	if (helpInfo.helpWanted) {
		defaultGetoptFormatter(stderr.lockingTextWriter, `
Load a dataset to shared memory.

Loaded with selected encoding to (default:) '/dev/shm/dataset.bin'; changes lines to null
terminated strings so that the file can be loaded as memory mapped file and
accessed as array.

File '/dev/shm/dataset_meta.bin' is an array of the offsets (u64[]); points to
the value past the end of the current string (like one is used to, chars of
string are indexed while i<offset):
datasets.bin: ['A', '\0', 'B', '\0']
datasets_meta.bin: [2, 4]
			`.strip ~ "\n\n", helpInfo.options, "%*s %-*s %*s%s\n");
		return 0;
	}

	enforce(outputName.length > 0, "output filename must not be empty");
	enforce(pathDataset != "", "no dataset given, see --help");

	auto f = [&loadFile!(Unicode.Utf_8),
		&loadFile!(Unicode.Utf_16),
		&loadFile!(Unicode.Utf_16_lex),
		&loadFile!(Unicode.Utf_32)][encoding];

	f(pathDataset, outputName, forceBigEndian);

	return 0;
}

enum Unicode : ubyte
{
	Utf_8,
	Utf_16,
	Utf_16_lex,
	Utf_32,
}


/+ Alg. to make UTF-16 comparable by code unit like UTF-8 exists for a while,
found this:
https://icu-project.org/docs/papers/utf16_code_point_order.html

Markus W. Scherer
Unicode Software Engineer, IBM
April 2000, updated August 2001
Originally published on http://www.ibm.com/developerWorks/unicode/
+/
void loadFile(Unicode encoding)(string pathDataset, string outputName,
	bool forceBigEndian)
{
	static if (encoding == Unicode.Utf_8)
		alias Char = char;
	else static if (encoding == Unicode.Utf_16
					|| encoding == Unicode.Utf_16_lex)
		alias Char = wchar;
	else static if (encoding == Unicode.Utf_32)
		alias Char = dchar;
	else
		static assert(0, "not implemented");

	auto finput = File(pathDataset);

	const pathData = "/dev/shm/" ~ outputName ~ ".bin";
	const pathMeta = "/dev/shm/" ~ outputName ~ "_meta.bin";
	auto fdata = File(pathData, "wb+x");
	auto fmeta = File(pathMeta, "wb+x");
	fdata.setvbuf(4 << 20);
	fmeta.setvbuf(4 << 20);

	ulong[] eos = [0]; // rawWrite only defined for []
	Appender!(Char[]) s;
	s.reserve(1024);
	Appender!(ubyte[]) bigEndian;
	bigEndian.reserve(1024);

	foreach(line; finput.byLine(No.keepTerminator))
	{
		if (line.length == 0)
			continue;
		s.clear();
		s ~= line;
		s ~= '\0';
		/+ NOTE(@Daniel): UTF_16_fake with LITTLE ENDIAN is needed when a data
		structure supports 16bit span but not std::wstring.
		+/
		static if (encoding == Unicode.Utf_16_lex)
		{
			foreach(ref codeUnit; s.data)
			{
				if (codeUnit >= 0xD800 && codeUnit < 0xE000) // is surrogate?
					codeUnit += 0x2000;
				else if (codeUnit >= 0xE000)
					codeUnit -= 0x0800;
			}
		}
		/+ NOTE(@Daniel): force big endian is only needed when span supports
		only 8bits.  In that case the encoding essentially becomes uint8_t[].
		+/
		if (forceBigEndian)
		{
			bigEndian.clear();
			foreach(i, c; s.data)
				bigEndian.put(nativeToBigEndian(c)[]);
			fdata.rawWrite(bigEndian.data);
			eos[0] += bigEndian.length;
			fmeta.rawWrite(eos);
			continue;
		}
		/+ NOTE(@Daniel): if std::wstring or uint32_t are available we can use
		native endianness.  If std::{,w,d}string are all available we have to
		consider the impact of Unicode collations which are ignored when using
		uint{8,16,32}_t.
		Use regular string length since here the reader parses the correct
		width.
		+/
		fdata.rawWrite(s.data);
		eos[0] += s.length;
		fmeta.rawWrite(eos);
	}
}
