// This file is a BUILTIN module that provides the actual implementation for the
// python-entrypoint.js USER module.

import { loadPyodide } from "pyodide-internal:python";
import { default as lockfile } from "pyodide-internal:generated/pyodide-lock.json";
import { default as MetadataReader } from "pyodide-internal:runtime-generated/metadata";


function initializePackageIndex(pyodide) {
  if (!lockfile.packages) {
    throw new Error(
      "Loaded pyodide lock file does not contain the expected key 'packages'.",
    );
  }
  const API = pyodide._api;
  const indexURL = `https://cdn.jsdelivr.net/pyodide/v${pyodide.version}/full/`;
  API.config.indexURL = indexURL;
  // TODO: explain here why we are setting `globalThis.location`
  globalThis.location = indexURL;
  API.lockfile_info = lockfile.info;
  API.lockfile_packages = lockfile.packages;
  API.repodata_packages = lockfile.packages;
  API.repodata_info = lockfile.info;
  API.lockfile_unvendored_stdlibs_and_test = [];

  // compute the inverted index for imports to package names
  API._import_name_to_package_name = new Map();
  for (let name of Object.keys(API.lockfile_packages)) {
    const pkg = API.lockfile_packages[name];

    for (let import_name of pkg.imports) {
      API._import_name_to_package_name.set(import_name, name);
    }

    if (pkg.package_type === "cpython_module") {
      API.lockfile_unvendored_stdlibs_and_test.push(name);
    }
  }

  API.lockfile_unvendored_stdlibs =
    API.lockfile_unvendored_stdlibs_and_test.filter((lib) => lib !== "test");

  const origLoadPackage = pyodide.loadPackage;
  pyodide.loadPackage = async function (packages, options) {
    // Must disable check integrity:
    // Subrequest integrity checking is not implemented. The integrity option
    // must be either undefined or an empty string.
    return await origLoadPackage(packages, {
      checkIntegrity: false,
      ...options,
    });
  };
}

// These packages are currently embedded inside EW and so don't need to
// be separately installed.
const EMBEDDED_PYTHON_PACKAGES = new Set([
  "aiohttp",
  "aiosignal",
  "anyio",
  "async_timeout",
  "attrs",
  "certifi",
  "charset_normalizer",
  "dataclasses_json",
  "distro",
  "fastapi",
  "frozenlist",
  "h11",
  "httpcore",
  "httpx",
  "idna",
  "jsonpatch",
  "jsonpointer",
  "langchain",
  "langchain_community",
  "langchain_core",
  "langsmith",
  "marshmallow",
  "micropip",
  "multidict",
  "mypy_extensions",
  "numpy",
  "openai",
  "packaging",
  "pydantic",
  "PyYAML",
  "requests",
  "setuptools",
  "sniffio",
  "SQLAlchemy",
  "starlette",
  "tenacity",
  "tqdm",
  "typing_extensions",
  "typing_inspect",
  "urllib3",
  "yarl",
]);

async function setupPackages(pyodide) {
  // The metadata is a JSON-serialised WorkerBundle (defined in pipeline.capnp).
  const isWorkerd = MetadataReader.isWorkerd();

  const pymajor = pyodide._module._py_version_major();
  const pyminor = pyodide._module._py_version_minor();
  const site_packages = `/lib/python${pymajor}.${pyminor}/site-packages`;

  initializePackageIndex(pyodide);

  const requirements = MetadataReader.getRequirements();
  const pythonRequirements = isWorkerd ? requirements : requirements.filter(req => !EMBEDDED_PYTHON_PACKAGES.has(req));

  if (pythonRequirements.length > 0) {
    // Our embedded packages tarball always contains at least `micropip` so we can load it here safely.
    const micropip = pyodide.pyimport("micropip");
    await micropip.install(pythonRequirements);
  }

  // Apply patches that enable some packages to work. We don't currently list
  // out transitive dependencies so these checks are very brittle.
  // TODO: Fix this
  if (
    requirements.includes("aiohttp") ||
    requirements.includes("openai") ||
    requirements.includes("langchain")
  ) {
    const mod = await import("pyodide-internal:patches/aiohttp_fetch_patch.py");
    pyodide.FS.writeFile(
      `${site_packages}/aiohttp_fetch_patch.py`,
      new Uint8Array(mod.default),
      { canOwn: true },
    );
    pyodide.pyimport("aiohttp_fetch_patch");
  }
  if (requirements.some((req) => req.startsWith("fastapi"))) {
    const mod = await import("pyodide-internal:asgi.py");
    pyodide.FS.writeFile(
      `${site_packages}/asgi.py`,
      new Uint8Array(mod.default),
      { canOwn: true },
    );
  }
  let mainModuleName = MetadataReader.getMainModule();
  if (mainModuleName.endsWith(".py")) {
    mainModuleName = mainModuleName.slice(0, -3);
  } else {
    throw new Error("Main module needs to end with a .py file extension");
  }
  return pyodide.pyimport(mainModuleName);
}

let mainModulePromise;
function getPyodide() {
  if (mainModulePromise !== undefined) {
    return mainModulePromise;
  }
  mainModulePromise = (async function() {
    // TODO: investigate whether it is possible to run part of loadPyodide in top level scope
    // When we do it in top level scope we seem to get a broken file system.
    const pyodide = await loadPyodide();
    const mainModule = await setupPackages(pyodide);
    return { mainModule };
  })();
  return mainModulePromise;
}

export default {
  async fetch(request, env, ctx) {
    const { mainModule } = await getPyodide();
    return await mainModule.fetch.callRelaxed(request, env, ctx);
  },
  async test(ctrl, env, ctx) {
    try {
      const { mainModule } = await getPyodide();
      return await mainModule.test.callRelaxed(ctrl, env, ctx);
    } catch (e) {
      console.warn(e);
      throw e;
    }
  },
};