"""Resolves map material names to base textures and render flags, by reading VMT files."""

from .vtf import Vtf


def parse_keyvalues(text):
    """Minimal KeyValues parser. Returns nested dicts with lower-cased keys; first value wins."""
    tokens = _tokenize(text)
    pos = 0

    def parse_block():
        nonlocal pos
        out = {}
        while pos < len(tokens):
            tok = tokens[pos]
            if tok == "}":
                pos += 1
                return out
            key = tok.lower()
            pos += 1
            if pos >= len(tokens):
                break
            if tokens[pos] == "{":
                pos += 1
                val = parse_block()
            else:
                val = tokens[pos]
                pos += 1
                # Platform conditionals such as [$X360] trail the value. Skip them.
                if pos < len(tokens) and tokens[pos].startswith("[") and tokens[pos].endswith("]"):
                    pos += 1
            out.setdefault(key, val)
        return out

    return parse_block()


def _tokenize(text):
    toks = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c in " \t\r\n":
            i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j + 1
        elif c in "{}":
            toks.append(c)
            i += 1
        elif c == '"':
            j = text.find('"', i + 1)
            if j < 0:
                j = n
            toks.append(text[i + 1 : j])
            i = j + 1
        else:
            j = i
            while j < n and text[j] not in " \t\r\n{}\"":
                j += 1
            toks.append(text[i:j])
            i = j
    return toks


class Material:
    def __init__(self, name):
        self.name = name
        self.shader = ""
        self.base_texture = None  # path under materials/, no extension
        self.alphatest = False
        self.translucent = False
        self.nocull = False
        self.water = False
        self.params = {}


def _truthy(v):
    try:
        return float(v) != 0.0
    except (TypeError, ValueError):
        return False


class MaterialLoader:
    def __init__(self, fs):
        self.fs = fs
        self._cache = {}

    def _load_vmt(self, path, depth=0):
        data = self.fs.read(path)
        if data is None or depth > 4:
            return None, None
        kv = parse_keyvalues(data.decode("latin-1"))
        if not kv:
            return None, None
        shader, params = next(iter(kv.items()))
        if not isinstance(params, dict):
            return shader, {}
        if shader == "patch":
            inc = params.get("include", "")
            base_shader, base = self._load_vmt(inc, depth + 1)
            if base is None:
                return None, None
            merged = dict(base)
            for section in ("insert", "replace"):
                extra = params.get(section)
                if isinstance(extra, dict):
                    merged.update(extra)
            return base_shader, merged
        return shader, params

    def get(self, name):
        key = name.lower().replace("\\", "/")
        if key in self._cache:
            return self._cache[key]
        if key.startswith("hue") and ":" in key:
            # Same material with its colours turned round the colour wheel (props.txt hue=N)
            deg, _, rest = key[3:].partition(":")
            src = self.get(rest)
            m = Material(key)
            for k, v in vars(src).items():
                setattr(m, k, v)
            m.name = key
            m.hue = float(deg)
            self._cache[key] = m
            return m
        if key.startswith("eye:"):
            # An eyeball with projected UVs (models.py): white with the iris in the centre half
            src = self.get(key[4:])
            m = Material(key)
            m.shader = src.shader
            m.params = src.params
            iris = src.params.get("$iris") if src.shader == "eyes" else None
            m.base_texture = (iris or src.base_texture or "").lower().replace("\\", "/") or None
            m.eye = True
            self._cache[key] = m
            return m
        m = Material(key)
        shader, params = self._load_vmt(f"materials/{key}.vmt")
        if params is not None:
            # Some shaders keep their real params in a fallback block such as "lightmappedgeneric_dx9".
            flat = dict(params)
            for v in params.values():
                if isinstance(v, dict):
                    for k2, v2 in v.items():
                        flat.setdefault(k2, v2)
            m.shader = (shader or "").lower()
            m.params = flat
            bt = flat.get("$basetexture")
            if isinstance(bt, str) and bt:
                m.base_texture = bt.lower().replace("\\", "/")
                if m.base_texture.endswith(".vtf"):
                    m.base_texture = m.base_texture[:-4]
            m.alphatest = _truthy(flat.get("$alphatest"))
            m.translucent = _truthy(flat.get("$translucent"))
            m.nocull = _truthy(flat.get("$nocull"))
            m.water = m.shader == "water"
        self._cache[key] = m
        return m

    def load_texture(self, mat, max_dim):
        """PIL RGBA image of the material's base texture at a mip no bigger than max_dim, or None."""
        if not mat.base_texture:
            return None
        data = self.fs.read(f"materials/{mat.base_texture}.vtf")
        if data is None:
            return None
        try:
            vtf = Vtf(data)
            if getattr(mat, "eye", False):
                return self._eye_image(vtf, max_dim)
            img = vtf.decode(vtf.pick_level(max_dim))
            if getattr(mat, "hue", 0.0):
                img = self._turn_hue(img, mat.hue)
            return img
        except (ValueError, OSError) as e:
            print(f"  warning: {mat.base_texture}.vtf: {e}")
            return None

    @staticmethod
    def _turn_hue(img, degrees):
        from PIL import Image

        img = img.convert("RGBA")
        alpha = img.getchannel("A")
        h, s, v = img.convert("RGB").convert("HSV").split()
        shift = int(round(degrees / 360.0 * 256)) % 256
        h = h.point(lambda x: (x + shift) % 256)
        out = Image.merge("HSV", (h, s, v)).convert("RGBA")
        out.putalpha(alpha)
        return out

    @staticmethod
    def _eye_image(vtf, max_dim):
        from PIL import Image

        iris = vtf.decode(vtf.pick_level(max_dim // 2)).convert("RGBA")
        size = max(iris.size) * 2
        iris = iris.resize((size // 2, size // 2))
        out = Image.new("RGBA", (size, size), (225, 220, 212, 255))
        out.alpha_composite(iris, (size // 4, size // 4))
        return out
