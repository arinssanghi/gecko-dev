/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const { XPCOMUtils } = ChromeUtils.import(
  "resource://gre/modules/XPCOMUtils.jsm"
);

XPCOMUtils.defineLazyGlobalGetters(this, ["indexedDB"]);

XPCOMUtils.defineLazyModuleGetters(this, {
  CommonUtils: "resource://services-common/utils.js",
});

var EXPORTED_SYMBOLS = ["Database"];

// IndexedDB name.
const DB_NAME = "remote-settings";
const DB_VERSION = 2;

/**
 * Wrap IndexedDB errors to catch them more easily.
 */
class IndexedDBError extends Error {
  constructor(error, method = "") {
    super(`IndexedDB: ${method} ${error.message}`);
    this.name = error.name;
    this.stack = error.stack;
  }
}

/**
 * Database is a tiny wrapper with the objective
 * of providing major kinto-offline-client collection API.
 * (with the objective of getting rid of kinto-offline-client)
 */
class Database {
  /* Expose the IDBError class publicly */
  static get IDBError() {
    return IndexedDBError;
  }

  constructor(identifier) {
    this.identifier = identifier;
    this._idb = null;
  }

  async open() {
    if (!this._idb) {
      // Open and initialize/upgrade if needed.
      this._idb = await openIDB(DB_NAME, DB_VERSION, event => {
        const db = event.target.result;
        if (event.oldVersion < 1) {
          // Records store
          const recordsStore = db.createObjectStore("records", {
            keyPath: ["_cid", "id"],
          });
          // An index to obtain all the records in a collection.
          recordsStore.createIndex("cid", "_cid");
          // Last modified field
          recordsStore.createIndex("last_modified", ["_cid", "last_modified"]);
          // Timestamps store
          db.createObjectStore("timestamps", {
            keyPath: "cid",
          });
        }
        if (event.oldVersion < 2) {
          // Collections store
          db.createObjectStore("collections", {
            keyPath: "cid",
          });
        }
      });
    }
    return this._idb;
  }

  async close() {
    if (this._idb) {
      this._idb.close();
    }
    this._idb = null;
  }

  async list(options = {}) {
    const { filters = {}, sort = "" } = options;
    const objFilters = transformSubObjectFilters(filters);
    let results = [];
    try {
      await executeIDB(await this.open(), "records", store => {
        const request = store
          .index("cid")
          .openCursor(IDBKeyRange.only(this.identifier));
        request.onsuccess = event => {
          const cursor = event.target.result;
          if (cursor) {
            const { value } = cursor;
            if (filterObject(objFilters, value)) {
              results.push(value);
            }
            cursor.continue();
          }
        };
      });
    } catch (e) {
      throw new IndexedDBError(e, "list()");
    }
    // Remove IDB key field from results.
    for (const result of results) {
      delete result._cid;
    }
    return sort ? sortObjects(sort, results) : results;
  }

  async importBulk(toInsert) {
    const _cid = this.identifier;
    try {
      await executeIDB(await this.open(), "records", store => {
        // Chain the put operations together, the last one will be waited by
        // the `transaction.oncomplete` callback.
        let i = 0;
        putNext();

        function putNext() {
          if (i == toInsert.length) {
            return;
          }
          const entry = { ...toInsert[i], _cid };
          store.put(entry).onsuccess = putNext; // On error, `transaction.onerror` is called.
          ++i;
        }
      });
    } catch (e) {
      throw new IndexedDBError(e, "importBulk()");
    }
  }

  async deleteBulk(toDelete) {
    const _cid = this.identifier;
    try {
      await executeIDB(await this.open(), "records", store => {
        // Chain the delete operations together, the last one will be waited by
        // the `transaction.oncomplete` callback.
        let i = 0;
        deleteNext();

        function deleteNext() {
          if (i == toDelete.length) {
            return;
          }
          store.delete([_cid, toDelete[i].id]).onsuccess = deleteNext; // On error, `transaction.onerror` is called.
          ++i;
        }
      });
    } catch (e) {
      throw new IndexedDBError(e, "deleteBulk()");
    }
  }

  async getLastModified() {
    let entry = null;
    try {
      await executeIDB(
        await this.open(),
        "timestamps",
        store => {
          store.get(this.identifier).onsuccess = e => (entry = e.target.result);
        },
        { mode: "readonly" }
      );
    } catch (e) {
      throw new IndexedDBError(e, "getLastModified()");
    }
    return entry ? entry.value : null;
  }

  async saveLastModified(lastModified) {
    const value = parseInt(lastModified, 10) || null;
    try {
      await executeIDB(await this.open(), "timestamps", store => {
        if (value === null) {
          store.delete(this.identifier);
        } else {
          store.put({ cid: this.identifier, value });
        }
      });
    } catch (e) {
      throw new IndexedDBError(e, "saveLastModified()");
    }
    return value;
  }

  async getMetadata() {
    let entry = null;
    try {
      await executeIDB(
        await this.open(),
        "collections",
        store => {
          store.get(this.identifier).onsuccess = e => (entry = e.target.result);
        },
        { mode: "readonly" }
      );
    } catch (e) {
      throw new IndexedDBError(e, "getMetadata()");
    }
    return entry ? entry.metadata : null;
  }

  async saveMetadata(metadata) {
    try {
      await executeIDB(await this.open(), "collections", store =>
        store.put({ cid: this.identifier, metadata })
      );
      return metadata;
    } catch (e) {
      throw new IndexedDBError(e, "saveMetadata()");
    }
  }

  async clear() {
    try {
      await this.saveLastModified(null);
      await this.saveMetadata(null);
      await executeIDB(await this.open(), "records", store => {
        const range = IDBKeyRange.only(this.identifier);
        const request = store.index("cid").openKeyCursor(range);
        request.onsuccess = event => {
          const cursor = event.target.result;
          if (cursor) {
            store.delete(cursor.primaryKey);
            cursor.continue();
          }
        };
        return request;
      });
    } catch (e) {
      throw new IndexedDBError(e, "clear()");
    }
  }

  /*
   * Methods used by unit tests.
   */

  async create(record) {
    if (!("id" in record)) {
      record = { ...record, id: CommonUtils.generateUUID() };
    }
    try {
      await executeIDB(await this.open(), "records", store => {
        store.add({ ...record, _cid: this.identifier });
      });
    } catch (e) {
      throw new IndexedDBError(e, "create()");
    }
    return record;
  }

  async update(record) {
    try {
      await executeIDB(await this.open(), "records", store => {
        store.put({ ...record, _cid: this.identifier });
      });
    } catch (e) {
      throw new IndexedDBError(e, "update()");
    }
  }

  async delete(recordId) {
    try {
      await executeIDB(await this.open(), "records", store => {
        store.delete([this.identifier, recordId]); // [_cid, id]
      });
    } catch (e) {
      throw new IndexedDBError(e, "delete()");
    }
  }
}

/**
 * Helper to wrap indexedDB.open() into a promise.
 */
async function openIDB(dbname, version, callback) {
  return new Promise((resolve, reject) => {
    const request = indexedDB.open(dbname, version);
    request.onupgradeneeded = event => {
      // When an upgrade is needed, a transaction is started.
      const transaction = event.target.transaction;
      transaction.onabort = event => {
        const error =
          event.target.error ||
          transaction.error ||
          new DOMException("The operation has been aborted", "AbortError");
        reject(new IndexedDBError(error));
      };

      const db = event.target.result;
      db.onerror = event => reject(new IndexedDBError(event.target.error));

      callback(event);
    };
    request.onerror = event => reject(new IndexedDBError(event.target.error));
    request.onsuccess = event => {
      const db = event.target.result;
      resolve(db);
    };
  });
}

/**
 * Helper to wrap some IDBObjectStore operations into a promise.
 *
 * @param {IDBDatabase} db
 * @param {String} storeName
 * @param {function} callback
 * @param {Object} options
 * @param {String} options.mode
 */
async function executeIDB(db, storeName, callback, options = {}) {
  const { mode = "readwrite" } = options;
  return new Promise((resolve, reject) => {
    const transaction = db.transaction([storeName], mode);
    const store = transaction.objectStore(storeName);
    let result;
    try {
      result = callback(store);
    } catch (e) {
      transaction.abort();
      reject(new IndexedDBError(e));
    }
    transaction.onerror = event =>
      reject(new IndexedDBError(event.target.error));
    transaction.oncomplete = event => resolve(result);
  });
}

function _isUndefined(value) {
  return typeof value === "undefined";
}

function makeNestedObjectFromArr(arr, val, nestedFiltersObj) {
  const last = arr.length - 1;
  return arr.reduce((acc, cv, i) => {
    if (i === last) {
      return (acc[cv] = val);
    } else if (Object.prototype.hasOwnProperty.call(acc, cv)) {
      return acc[cv];
    }
    return (acc[cv] = {});
  }, nestedFiltersObj);
}

function transformSubObjectFilters(filtersObj) {
  const transformedFilters = {};
  for (const [key, val] of Object.entries(filtersObj)) {
    const keysArr = key.split(".");
    makeNestedObjectFromArr(keysArr, val, transformedFilters);
  }
  return transformedFilters;
}

/**
 * Test if a single object matches all given filters.
 *
 * @param  {Object} filters  The filters object.
 * @param  {Object} entry    The object to filter.
 * @return {Boolean}
 */
function filterObject(filters, entry) {
  return Object.entries(filters).every(([filter, value]) => {
    if (Array.isArray(value)) {
      return value.some(candidate => candidate === entry[filter]);
    } else if (typeof value === "object") {
      return filterObject(value, entry[filter]);
    } else if (!Object.prototype.hasOwnProperty.call(entry, filter)) {
      console.error(`The property ${filter} does not exist`);
      return false;
    }
    return entry[filter] === value;
  });
}

/**
 * Sorts records in a list according to a given ordering.
 *
 * @param  {String} order The ordering, eg. `-last_modified`.
 * @param  {Array}  list  The collection to order.
 * @return {Array}
 */
function sortObjects(order, list) {
  const hasDash = order[0] === "-";
  const field = hasDash ? order.slice(1) : order;
  const direction = hasDash ? -1 : 1;
  return list.slice().sort((a, b) => {
    if (a[field] && _isUndefined(b[field])) {
      return direction;
    }
    if (b[field] && _isUndefined(a[field])) {
      return -direction;
    }
    if (_isUndefined(a[field]) && _isUndefined(b[field])) {
      return 0;
    }
    return a[field] > b[field] ? direction : -direction;
  });
}
