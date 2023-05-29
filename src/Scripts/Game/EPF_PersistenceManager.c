enum EPF_EPersistenceManagerState
{
	PRE_INIT,
	POST_INIT,
	SETUP,
	ACTIVE,
	SHUTDOWN
};

class EPF_PersistenceManager
{
	protected static ref EPF_PersistenceManager s_pInstance;

	protected ref EPF_PersistenceManagerComponentClass m_pSettings;

	// Startup and shutdown sequence
	protected EPF_EPersistenceManagerState m_eState;
	protected ref ScriptInvoker m_pOnStateChange;

	// Underlying database connection
	protected ref EDF_DbContext m_pDbContext;

	// Instance tracking
	protected ref array<EPF_PersistenceComponent> m_aPendingEntityRegistrations;
	protected ref array<EPF_PersistentScriptedState> m_aPendingScriptedStateRegistrations;
	protected ref EPF_PersistentRootEntityCollection m_pRootEntityCollection;
	protected ref map<string, EPF_PersistenceComponent> m_mRootAutoSave;
	protected ref map<string, typename> m_mRootAutoSaveCleanup;
	protected ref map<string, EPF_PersistenceComponent> m_mRootShutdown;
	protected ref map<string, typename> m_mRootShutdownCleanup;
	protected ref map<string, EPF_PersistenceComponent> m_mUncategorizedEntities;
	protected ref map<string, EPF_PersistentScriptedState> m_mScriptedStateAutoSave;
	protected ref map<string, EPF_PersistentScriptedState> m_mScriptedStateShutdown;
	protected ref map<string, EPF_PersistentScriptedState> m_mScriptedStateUncategorized;

	// Auto save system
	protected float m_fAutoSaveAccumultor;
	protected bool m_bAutoSaveActive;
	protected int m_iSaveOperation;
	protected MapIterator m_iAutoSaveEntityIt;
	protected MapIterator m_iAutoSaveScriptedStateIt;

	// Setup buffers, discarded after world init
	protected ref map<string, EPF_PersistenceComponent> m_mBakedRoots;

	//------------------------------------------------------------------------------------------------
	//! Check if current game instance is intended to run the persistence system. Only the mission host should do so.
	//! \return true if persistence should be run, false otherwise.
	static bool IsPersistenceMaster()
	{
		if (!Replication.IsServer())
			return false;

		ArmaReforgerScripted game = GetGame();
		return game && game.InPlayMode();
	}

	//------------------------------------------------------------------------------------------------
	//! Get the singleton instance of the persistence manager
	//! \param create Create the singleton if not yet existing
	//! \return persistence manager instance or null if game instance role does not allow persistence handling or the instance did not exist and create was disabled
	static EPF_PersistenceManager GetInstance(bool create = true)
	{
		// Persistence logic only runs on the server machine
		if (!IsPersistenceMaster())
			return null;

		if (!s_pInstance && create)
		{
			s_pInstance = new EPF_PersistenceManager();

			//Reset the singleton when a new mission is loaded to free all memory and have a clean startup again.
			GetGame().m_OnMissionSetInvoker.Insert(Reset);
		}

		return s_pInstance;
	}

	//------------------------------------------------------------------------------------------------
	protected void SetState(EPF_EPersistenceManagerState state)
	{
		m_eState = state;
		if (m_pOnStateChange) m_pOnStateChange.Invoke(this, state);
	}

	//------------------------------------------------------------------------------------------------
	EPF_EPersistenceManagerState GetState()
	{
		return m_eState;
	}

	//------------------------------------------------------------------------------------------------
	//! Get the event invoker that can be subscribed to be notified about persistence manager state/phase changes.
	ScriptInvoker GetOnStateChangeEvent()
	{
		if (!m_pOnStateChange) m_pOnStateChange = new ScriptInvoker();
		return m_pOnStateChange;
	}

	//------------------------------------------------------------------------------------------------
	//! Get the database context that is used by the persistence system
	//! \return database context instance
	EDF_DbContext GetDbContext()
	{
		return m_pDbContext;
	}

	//------------------------------------------------------------------------------------------------
	//! Used to spawn and correctly register an entity from save-data
	//! \param saveData Save-data to spawn from
	//! \param isRoot true if the current entity is a world root (not a stored item inside a storage)
	//! \return registered entiy instance or null on failure
	IEntity SpawnWorldEntity(EPF_EntitySaveData saveData, bool isRoot = true)
	{
		if (!saveData || !saveData.GetId())
			return null;

		Resource resource = Resource.Load(saveData.m_rPrefab);
		if (!resource.IsValid())
		{
			Debug.Error(string.Format("Invalid prefab type '%1' on '%2:%3' could not be spawned. Ignored.", saveData.m_rPrefab, saveData.Type().ToString(), saveData.GetId()));
			return null;
		}

		IEntity entity = GetGame().SpawnEntityPrefab(resource);
		if (!entity)
		{
			Debug.Error(string.Format("Failed to spawn entity '%1:%2'. Ignored.", saveData.Type().ToString(), saveData.GetId()));
			return null;
		}

		EPF_PersistenceComponent persistenceComponent = EPF_PersistenceComponent.Cast(entity.FindComponent(EPF_PersistenceComponent));
		if (!persistenceComponent || !persistenceComponent.Load(saveData, isRoot))
		{
			SCR_EntityHelper.DeleteEntityAndChildren(entity);
			return null;
		}

		return entity;
	}

	//------------------------------------------------------------------------------------------------
	//! Used to instantiate and correctly register a scripted state from save-data
	//! \param saveData Save-data to instantiate from
	//! \return registered scripted state instance or null on failure
	EPF_PersistentScriptedState SpawnScriptedState(EPF_ScriptedStateSaveData saveData)
	{
		if (!saveData || !saveData.GetId())
			return null;

		typename scriptedStateType = EPF_PersistentScriptedStateSettings.GetScriptedStateType(saveData.Type());
		if (!scriptedStateType.IsInherited(EPF_PersistentScriptedState))
		{
			Debug.Error(string.Format("Can not spawn '%1' because it is only a proxy for '%2' instances. Use EPF_PersistentScriptedState.CreateProxy instead.", saveData.ClassName(), scriptedStateType.ToString()));
			return null;
		}

		EPF_PersistentScriptedState state = EPF_PersistentScriptedState.Cast(scriptedStateType.Spawn());
		if (!state)
		{
			Debug.Error(string.Format("Failed to spawn scripted state '%1:%2'. Ignored.", saveData.Type().ToString(), saveData.GetId()));
			return null;
		}

		if (!state.Load(saveData))
			return null;

		return state;
	}

	//------------------------------------------------------------------------------------------------
	//! Find an entity that is registered in the persistence system by its persistent id
	//! \return entity instance or null if not found
	IEntity FindEntityByPersistentId(string persistentId)
	{
		EPF_PersistenceComponent result = FindPersistenceComponentById(persistentId);
		if (result) return result.GetOwner();
		return null;
	}

	//------------------------------------------------------------------------------------------------
	//! Find a peristence component that is registered in the persistence system by its id
	//! \return entity instance or null if not found
	EPF_PersistenceComponent FindPersistenceComponentById(string persistentId)
	{
		FlushRegistrations();
		EPF_PersistenceComponent result;
		if (m_mRootAutoSave.Find(persistentId, result)) return result;
		if (m_mRootShutdown.Find(persistentId, result)) return result;
		return m_mUncategorizedEntities.Get(persistentId);
	}

	//------------------------------------------------------------------------------------------------
	//! Find a scripted state that is registered in the persistence system by its persistent id
	//! \return scripted state instance or null if not found
	EPF_PersistentScriptedState FindScriptedStateByPersistentId(string persistentId)
	{
		FlushRegistrations();
		EPF_PersistentScriptedState result;
		if (m_mScriptedStateAutoSave.Find(persistentId, result)) return result;
		if (m_mScriptedStateShutdown.Find(persistentId, result)) return result;
		return m_mScriptedStateUncategorized.Get(persistentId);
	}

	//------------------------------------------------------------------------------------------------
	//! Manually trigger the global auto-save. Resets the timer until the next auto-save cycle. If an auto-save is already in progress it will do nothing.
	void AutoSave()
	{
		if (m_bAutoSaveActive)
			return;

		m_bAutoSaveActive = true;
		m_fAutoSaveAccumultor = 0;
		m_iSaveOperation = 0;

		FlushRegistrations();

		// Need to get iterators afterwards as they might change
		m_iAutoSaveEntityIt = m_mRootAutoSave.Begin();
		m_iAutoSaveScriptedStateIt = m_mScriptedStateAutoSave.Begin();

		Print("Persistence auto-save started ...", LogLevel.DEBUG);
	}

	//------------------------------------------------------------------------------------------------
	protected void AutoSaveTick()
	{
		if (!m_bAutoSaveActive)
			return;

		MapIterator mapEnd = m_mRootAutoSave.End();
		while (m_iAutoSaveEntityIt != mapEnd)
		{
			EPF_PersistenceComponent persistenceComponent = m_mRootAutoSave.GetIteratorElement(m_iAutoSaveEntityIt);
			m_iAutoSaveEntityIt = m_mRootAutoSave.Next(m_iAutoSaveEntityIt);

			if (EPF_BitFlags.CheckFlags(persistenceComponent.GetFlags(), EPF_EPersistenceFlags.PAUSE_TRACKING))
				continue;

			persistenceComponent.Save();
			m_iSaveOperation++;

			if ((m_eState == EPF_EPersistenceManagerState.ACTIVE) &&
				((m_iSaveOperation + 1) % m_pSettings.m_iAutosaveIterations == 0))
			{
				return; // Pause execution until next tick
			}
		}

		while (m_iAutoSaveScriptedStateIt != m_mScriptedStateAutoSave.End())
		{
			EPF_PersistentScriptedState scriptedState = m_mScriptedStateAutoSave.GetIteratorElement(m_iAutoSaveScriptedStateIt);
			m_iAutoSaveScriptedStateIt = m_mScriptedStateAutoSave.Next(m_iAutoSaveScriptedStateIt);

			if (EPF_BitFlags.CheckFlags(scriptedState.GetFlags(), EPF_EPersistenceFlags.PAUSE_TRACKING))
				continue;

			scriptedState.Save();
			m_iSaveOperation++;

			if ((m_eState == EPF_EPersistenceManagerState.ACTIVE) &&
				((m_iSaveOperation + 1) % m_pSettings.m_iAutosaveIterations == 0))
			{
				return; // Pause execution until next tick
			}
		}

		m_pRootEntityCollection.Save(m_pDbContext);

		// Remove records about former root enties that were not purged by a persistent parent's recursive save.
		foreach (string persistentId, typename saveDataTypename : m_mRootAutoSaveCleanup)
		{
			m_pDbContext.RemoveAsync(saveDataTypename, persistentId);
		}
		m_mRootAutoSaveCleanup.Clear();

		m_bAutoSaveActive = false;

		//FlushDatabase();

		Print("Persistence auto-save complete.", LogLevel.DEBUG);
	}

	//------------------------------------------------------------------------------------------------
	protected void ShutDownSave()
	{
		Print("Persistence shutdown-save started...", LogLevel.DEBUG);

		FlushRegistrations();

		foreach (auto _, EPF_PersistenceComponent persistenceComponent : m_mRootShutdown)
		{
			if (EPF_BitFlags.CheckFlags(persistenceComponent.GetFlags(), EPF_EPersistenceFlags.PAUSE_TRACKING)) continue;
			persistenceComponent.Save();
		}

		foreach (auto _, EPF_PersistentScriptedState scriptedState : m_mScriptedStateShutdown)
		{
			if (EPF_BitFlags.CheckFlags(scriptedState.GetFlags(), EPF_EPersistenceFlags.PAUSE_TRACKING)) continue;
			scriptedState.Save();
		}

		m_pRootEntityCollection.Save(m_pDbContext);

		// Remove records about former root enties that were not purged by a persistent parent's recursive save.
		foreach (string persistentId, typename saveDataTypename : m_mRootShutdownCleanup)
		{
			m_pDbContext.RemoveAsync(saveDataTypename, persistentId);
		}
		m_mRootShutdownCleanup.Clear();

		//FlushDatabase();

		Print("Persistence shutdown-save complete.", LogLevel.DEBUG);
	}

	//------------------------------------------------------------------------------------------------
	//! Get the persistent id for entity based on baked map hash or generate a dynamic one
	protected string GetPersistentId(notnull EPF_PersistenceComponent persistenceComponent)
	{
		// Baked
		if (m_eState < EPF_EPersistenceManagerState.SETUP)
			return EPF_PersistenceIdGenerator.Generate(persistenceComponent.GetOwner());

		return EPF_PersistenceIdGenerator.Generate();
	}

	//------------------------------------------------------------------------------------------------
	//! Get the scripted state persistent id that is dynamically generated
	protected string GetPersistentId(notnull EPF_PersistentScriptedState scripedState)
	{
		return EPF_PersistenceIdGenerator.Generate();
	}

	//------------------------------------------------------------------------------------------------
	//! Set the db context manually, mainly used by testing framework
	void SetDbContext(notnull EDF_DbContext dbContext)
	{
		m_pDbContext = dbContext;
	}

	//------------------------------------------------------------------------------------------------
	//! Enqueue entity for persistence registration for later (will be flushed before any find by id or save operation takes place)
	void EnqueueRegistration(notnull EPF_PersistenceComponent persistenceComponent)
	{
		m_aPendingEntityRegistrations.Insert(persistenceComponent);
	}

	//------------------------------------------------------------------------------------------------
	//! Register the persistence component to assign it's id and make it searchable.
	string Register(notnull EPF_PersistenceComponent persistenceComponent, string id = string.Empty)
	{
		IEntity owner = persistenceComponent.GetOwner();
		if (!owner)
			return string.Empty;

		if (!id)
			id = GetPersistentId(persistenceComponent);

		// "Hacky" restore of baked status.
		// If id no longer provides this info, record all ids created up until world init and remember those as baked to check here.
		if (id.StartsWith("0000"))
			persistenceComponent.FlagAsBaked();

		EPF_PersistenceComponentClass settings = EPF_ComponentData<EPF_PersistenceComponentClass>.Get(persistenceComponent);
		bool isRoot = EPF_BitFlags.CheckFlags(persistenceComponent.GetFlags(), EPF_EPersistenceFlags.ROOT);
		UpdateRootStatus(persistenceComponent, id, settings, isRoot);

		return id;
	}

	//------------------------------------------------------------------------------------------------
	//! id param seperate as it might not be assigned yet onto the persistence componend during registration
	void UpdateRootStatus(notnull EPF_PersistenceComponent persistenceComponent, string id, EPF_PersistenceComponentClass settings, bool isRootEntity)
	{
		switch (settings.m_eSaveType)
		{
			case EPF_ESaveType.INTERVAL_SHUTDOWN:
			{
				if (isRootEntity)
				{
					m_mUncategorizedEntities.Remove(id);
					m_mRootAutoSaveCleanup.Remove(id);
					m_mRootAutoSave.Set(id, persistenceComponent);
				}
				else
				{
					if (m_mRootAutoSave.Contains(id))
					{
						m_mRootAutoSave.Remove(id);

						if (EPF_BitFlags.CheckFlags(persistenceComponent.GetFlags(), EPF_EPersistenceFlags.PERSISTENT_RECORD))
							m_mRootAutoSaveCleanup.Set(id, settings.m_tSaveDataType);
					}
					m_mUncategorizedEntities.Set(id, persistenceComponent);
				}
				break;
			}

			case EPF_ESaveType.SHUTDOWN:
			{
				if (isRootEntity)
				{
					m_mUncategorizedEntities.Remove(id);
					m_mRootShutdownCleanup.Remove(id);
					m_mRootShutdown.Set(id, persistenceComponent);
				}
				else
				{
					if (m_mRootShutdown.Contains(id))
					{
						m_mRootShutdown.Remove(id);

						if (EPF_BitFlags.CheckFlags(persistenceComponent.GetFlags(), EPF_EPersistenceFlags.PERSISTENT_RECORD))
							m_mRootShutdownCleanup.Set(id, settings.m_tSaveDataType);
					}
					m_mUncategorizedEntities.Set(id, persistenceComponent);
				}
				break;
			}

			default:
			{
				m_mUncategorizedEntities.Set(id, persistenceComponent);
				break;
			}
		}

		UpdateRootEntityCollection(persistenceComponent, id, isRootEntity);
	}

	//------------------------------------------------------------------------------------------------
	void UpdateRootEntityCollection(notnull EPF_PersistenceComponent persistenceComponent, string id, bool isRootEntity)
	{
		if (m_eState < EPF_EPersistenceManagerState.ACTIVE)
		{
			if (m_mBakedRoots && EPF_BitFlags.CheckFlags(persistenceComponent.GetFlags(), EPF_EPersistenceFlags.BAKED))
			{
				if (isRootEntity)
				{
					m_mBakedRoots.Set(id, persistenceComponent);
				}
				else
				{
					m_mBakedRoots.Remove(id);
				}
			}

			return;
		}

		if (isRootEntity)
		{
			m_pRootEntityCollection.Add(persistenceComponent, id, m_eState);
		}
		else
		{
			m_pRootEntityCollection.Remove(persistenceComponent, id, m_eState);
		}
	}

	//------------------------------------------------------------------------------------------------
	void ForceSelfSpawn(notnull EPF_PersistenceComponent persistenceComponent)
	{
		EPF_PersistenceComponentClass settings = EPF_ComponentData<EPF_PersistenceComponentClass>.Get(persistenceComponent);
		m_pRootEntityCollection.ForceSelfSpawn(persistenceComponent, persistenceComponent.GetPersistentId(), settings);
	}

	//------------------------------------------------------------------------------------------------
	void Unregister(notnull EPF_PersistenceComponent persistenceComponent)
	{
		string id = persistenceComponent.GetPersistentId();
		m_mRootAutoSave.Remove(id);
		m_mRootShutdown.Remove(id);
		m_mUncategorizedEntities.Remove(id);
	}

	//------------------------------------------------------------------------------------------------
	void AddOrUpdateAsync(notnull EPF_EntitySaveData saveData)
	{
		m_pDbContext.AddOrUpdateAsync(saveData);
	}

	//------------------------------------------------------------------------------------------------
	void RemoveAsync(typename saveDataType, string id)
	{
		m_mRootAutoSaveCleanup.Remove(id);
		m_mRootShutdownCleanup.Remove(id);
		m_pDbContext.RemoveAsync(saveDataType, id);
	}

	//------------------------------------------------------------------------------------------------
	//! Enqueue scripted stat for persistence registration for later (will be flushed before any find by id or save operation takes place)
	void EnqueueRegistration(notnull EPF_PersistentScriptedState scripedState)
	{
		m_aPendingScriptedStateRegistrations.Insert(scripedState);
	}

	//------------------------------------------------------------------------------------------------
	//! Register the scripted state to assign it's id, subscribe to save events and make it searchable.
	string Register(notnull EPF_PersistentScriptedState scripedState, string id = string.Empty)
	{
		if (!id) id = GetPersistentId(scripedState);

		Managed target;
		EPF_PersistentScriptedStateProxy proxy = EPF_PersistentScriptedStateProxy.Cast(scripedState);
		if (proxy)
			target = proxy.m_pProxyTarget;

		if (!target)
			target = scripedState;

		EPF_PersistentScriptedStateSettings settings = EPF_PersistentScriptedStateSettings.Get(target.Type());

		if (settings.m_eSaveType == EPF_ESaveType.INTERVAL_SHUTDOWN)
		{
			m_mRootAutoSaveCleanup.Remove(id);
			m_mScriptedStateAutoSave.Set(id, scripedState);
		}
		else if (settings.m_eSaveType == EPF_ESaveType.SHUTDOWN)
		{
			m_mRootShutdownCleanup.Remove(id);
			m_mScriptedStateShutdown.Set(id, scripedState);
		}
		else
		{
			m_mScriptedStateUncategorized.Set(id, scripedState);
		}

		return id;
	}

	//------------------------------------------------------------------------------------------------
	void Unregister(notnull EPF_PersistentScriptedState scripedState)
	{
		string id = scripedState.GetPersistentId();
		m_mScriptedStateAutoSave.Remove(id);
		m_mScriptedStateShutdown.Remove(id);
		m_mScriptedStateUncategorized.Remove(id);
	}

	//------------------------------------------------------------------------------------------------
	/*
	void FlushDatabase()
	{
		EPF_BufferedDbContext bufferedDb = EPF_BufferedDbContext.Cast(m_pDbContext);
		if (bufferedDb)
			bufferedDb.Flush(m_pSettings.m_bBufferedDatabaseBatchsize);
	}
	*/

	//------------------------------------------------------------------------------------------------
	protected void FlushRegistrations()
	{
		// Ask for persistent ids. If they were already registerd, they will have them, if not they are registered now.
		if (m_eState >= EPF_EPersistenceManagerState.POST_INIT)
		{
			foreach (EPF_PersistenceComponent persistenceComponent : m_aPendingEntityRegistrations)
			{
				if (persistenceComponent)
					persistenceComponent.GetPersistentId();
			}
			m_aPendingEntityRegistrations.Clear();
		}

		foreach (EPF_PersistentScriptedState scripedState : m_aPendingScriptedStateRegistrations)
		{
			if (scripedState)
				scripedState.GetPersistentId();
		}
		m_aPendingScriptedStateRegistrations.Clear();
	}

	//------------------------------------------------------------------------------------------------
	event void OnPostInit(IEntity gameMode, EPF_PersistenceManagerComponentClass settings)
	{
		m_pSettings = settings;

		/*if (settings.m_bBufferedDatabaseContext)
		{
			m_pDbContext = EPF_BufferedDbContext.Create(settings.m_pConnectionInfo);
		}
		else
		{*/
			m_pDbContext = EDF_DbContext.Create(settings.m_pConnectionInfo);
		//}

		if (!m_pDbContext)
			return;

		string rootEntityCollectionId = string.Format("00EC%1-0000-0000-0000-000000000000", EPF_PersistenceIdGenerator.GetHiveId().ToString(4));
		m_pRootEntityCollection = EDF_DbEntityHelper<EPF_PersistentRootEntityCollection>.GetRepository(m_pDbContext).Find(rootEntityCollectionId).GetEntity();
		if (!m_pRootEntityCollection)
		{
			m_pRootEntityCollection = new EPF_PersistentRootEntityCollection();
			m_pRootEntityCollection.SetId(rootEntityCollectionId);
		}

		SetState(EPF_EPersistenceManagerState.POST_INIT);
	}

	//------------------------------------------------------------------------------------------------
	event void OnPostFrame(float timeSlice)
	{
		if (!m_pSettings.m_bEnableAutosave)
			return;

		m_fAutoSaveAccumultor += timeSlice;

		if (m_fAutoSaveAccumultor >= m_pSettings.m_fAutosaveInterval)
			AutoSave();

		AutoSaveTick();
	}

	//------------------------------------------------------------------------------------------------
	event void OnWorldPostProcess(World world)
	{
		Print("Persistence initial world load started...", LogLevel.DEBUG);

		FlushRegistrations();
		SetState(EPF_EPersistenceManagerState.SETUP);

		// Remove baked entities that shall no longer be root entities in the world
		array<string> staleIds();
		foreach (string persistentId : m_pRootEntityCollection.m_aRemovedBackedRootEntities)
		{
			IEntity entity = FindEntityByPersistentId(persistentId);
			if (!entity)
			{
				staleIds.Insert(persistentId);
				continue;
			}

			// Add here as the entity will be removed from m_mBakedRoots during dtor, so can't find the id again later
			m_pRootEntityCollection.m_aPossibleBackedRootEntities.Insert(persistentId);

			Print(string.Format("Deleting baked entity '%1'@%2.", EPF_Utils.GetPrefabName(entity), entity.GetOrigin()), LogLevel.VERBOSE);
			SCR_EntityHelper.DeleteEntityAndChildren(entity);
		}

		// Remove any removal entries for baked objects that no longer exist
		foreach (string staleId : staleIds)
		{
			m_pRootEntityCollection.m_aRemovedBackedRootEntities.RemoveItem(staleId);
		}

		// Collect type and ids of initial world entities for bulk load
		map<typename, ref array<string>> bulkLoad();
		foreach (typename saveType, array<string> persistentIds : m_pRootEntityCollection.m_mSelfSpawnDynamicEntities)
		{
			array<string> loadIds();
			loadIds.Copy(persistentIds);
			bulkLoad.Set(saveType, loadIds);
		}
		foreach (string id, EPF_PersistenceComponent persistenceComponent : m_mBakedRoots)
		{
			// Remember which ids were world roots on load finish so only those are removed on parent change.
			m_pRootEntityCollection.m_aPossibleBackedRootEntities.Insert(id);

			EPF_PersistenceComponentClass settings = EPF_ComponentData<EPF_PersistenceComponentClass>.Get(persistenceComponent);
			array<string> loadIds = bulkLoad.Get(settings.m_tSaveDataType);

			if (!loadIds)
			{
				loadIds = {};
				bulkLoad.Set(settings.m_tSaveDataType, loadIds);
			}

			loadIds.Insert(id);
		}

		// Load all known initial entity types from db, both baked and dynamic in one bulk operation
		foreach (typename saveDataType, array<string> persistentIds : bulkLoad)
		{
			array<ref EDF_DbEntity> findResults = m_pDbContext.FindAll(saveDataType, EDF_DbFind.Id().EqualsAnyOf(persistentIds)).GetEntities();
			if (!findResults) continue;

			foreach (EDF_DbEntity findResult : findResults)
			{
				EPF_EntitySaveData saveData = EPF_EntitySaveData.Cast(findResult);
				if (!saveData)
				{
					Debug.Error(string.Format("Unexpected database find result type '%1' encountered during entity load. Ignored.", findResult.Type().ToString()));
					continue;
				}

				// Load data for baked roots
				EPF_PersistenceComponent persistenceComponent = m_mBakedRoots.Get(saveData.GetId());
				if (persistenceComponent)
				{
					persistenceComponent.Load(saveData);
					continue;
				}

				// Spawn additional dynamic entites
				SpawnWorldEntity(saveData);
			}
		}

		// Save any mapping or root entity changes detected during world init
		m_pRootEntityCollection.Save(m_pDbContext);

		// Free memory as it not needed after setup
		m_mBakedRoots = null;

		SetState(EPF_EPersistenceManagerState.ACTIVE);

		Print("Persistence initial world load complete.", LogLevel.DEBUG);
	}

	//------------------------------------------------------------------------------------------------
	event void OnGameEnd()
	{
		Print("Persistence shutting down...", LogLevel.DEBUG);
		SetState(EPF_EPersistenceManagerState.SHUTDOWN);
		AutoSave(); // Trigger auto-save
		AutoSaveTick(); // Execute auto-save instantly till end
		ShutDownSave(); // Save those who only save on shutdown
		Reset();
		Print("Persistence shut down successfully.", LogLevel.DEBUG);
	}

	//------------------------------------------------------------------------------------------------
	protected void EPF_PersistenceManager()
	{
		m_aPendingEntityRegistrations = {};
		m_aPendingScriptedStateRegistrations = {};
		m_mRootAutoSave = new map<string, EPF_PersistenceComponent>();
		m_mRootAutoSaveCleanup = new map<string, typename>();
		m_mRootShutdown = new map<string, EPF_PersistenceComponent>();
		m_mRootShutdownCleanup = new map<string, typename>();
		m_mUncategorizedEntities = new map<string, EPF_PersistenceComponent>();
		m_mScriptedStateAutoSave = new map<string, EPF_PersistentScriptedState>();
		m_mScriptedStateShutdown = new map<string, EPF_PersistentScriptedState>();
		m_mScriptedStateUncategorized = new map<string, EPF_PersistentScriptedState>();
		m_mBakedRoots = new map<string, EPF_PersistenceComponent>();
	}

	//------------------------------------------------------------------------------------------------
	protected static void Reset()
	{
		EPF_EntitySlotPrefabInfo.Reset();
		EPF_StorageChangeDetection.Reset();
		EPF_PersistenceIdGenerator.Reset();
		EPF_PersistentScriptedStateProxy.s_mProxies = null;
		s_pInstance = null;
	}
};
