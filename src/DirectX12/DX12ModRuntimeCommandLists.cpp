static bool CommandListRuntimeEffectLocked(const std::wstring &name)
{
	auto it = gCommandListRuntimeEffect.find(name);
	return it == gCommandListRuntimeEffect.end() || it->second;
}

static DX12IaHashState EmptyIaHashState()
{
	DX12IaHashState state = {};
	return state;
}

class DX12CommandListExecutor
{
public:
	DX12CommandListExecutor(
		ID3D12Device *device,
		ID3D12GraphicsCommandList *commandList,
		const DX12IaHashState &iaState,
		uint32_t vertexCount, uint32_t indexCount, uint32_t instanceCount,
		uint32_t firstVertex, uint32_t firstIndex,
		DX12ModIaReplacement *replacement)
		: mDevice(device),
		  mCommandList(commandList),
		  mIaState(iaState),
		  mVertexCount(vertexCount),
		  mIndexCount(indexCount),
		  mInstanceCount(instanceCount),
		  mFirstVertex(firstVertex),
		  mFirstIndex(firstIndex),
		  mReplacement(replacement)
	{
	}

	void SetExecutionMode(bool executeCommands, bool executeDrawActions)
	{
		mExecuteCommands = executeCommands;
		mExecuteDrawActions = executeDrawActions;
	}

	bool Changed() const
	{
		return mChanged;
	}

	void ClearChanged()
	{
		mChanged = false;
	}

	void MarkExternalChange()
	{
		MarkChanged();
	}

	void RunTextureOverride(
		const Bunny::TextureOverrideConfig &config, bool includePost)
	{
		RunTextureOverride(config, 0, includePost);
	}

	void RunCommandListLinks(
		const Bunny::CommandListLinks &links, bool includePost)
	{
		RunCommandListLinks(links, 0, includePost);
	}

	void RunShaderOverride(const Bunny::ShaderOverrideConfig &config)
	{
		if (!mReplacement)
			return;
		if (config.handlingSkip)
			SetSkip();
		RunCommandListLinks(config.commandLists, 0, false);
		RunActions(config.actions, 0, false);
	}

	void RunPostTextureOverrideLists(
		const Bunny::TextureOverrideConfig &config, bool postRuntimeEffect)
	{
		if (!postRuntimeEffect)
			return;
		for (const std::wstring &list : config.commandLists.post)
			mReplacement->AddPostCommandList(list);
	}

	void RunPostShaderOverrideLists(
		const Bunny::ShaderOverrideConfig &config)
	{
		Bunny::CommandListLinks postLinks;
		postLinks.main = config.commandLists.post;
		RunCommandListLinks(postLinks, 0, true);
	}

private:
	void MarkChanged()
	{
		mChanged = true;
	}

	void SetSkip()
	{
		if (mReplacement)
			mReplacement->skip = true;
	}

	void AppendDraw(const Bunny::CommandListAction &action)
	{
		if (!mReplacement)
			return;
		DX12ModIaReplacement::DrawCall draw;
		draw.indexed =
			action.kind == Bunny::CommandListActionKind::DrawIndexed ||
			action.kind == Bunny::CommandListActionKind::DrawIndexedFromCaller;
		draw.fromCaller =
			action.kind == Bunny::CommandListActionKind::DrawFromCaller ||
			action.kind == Bunny::CommandListActionKind::DrawIndexedFromCaller;
		if (!draw.fromCaller) {
			draw.count = action.args[0];
			draw.start = action.args[1];
			draw.baseVertex = draw.indexed ? static_cast<INT>(action.args[2]) : 0;
		}
		mReplacement->draws.push_back(draw);
		MarkChanged();
	}

	void AppendDispatch(const Bunny::CommandListAction &action)
	{
		if (!mReplacement)
			return;
		DX12ModIaReplacement::DispatchCall dispatch;
		dispatch.groupsX = action.args[0];
		dispatch.groupsY = action.args[1];
		dispatch.groupsZ = action.args[2];
		mReplacement->dispatches.push_back(dispatch);
		MarkChanged();
	}

	void AppendResourceViews(
		const std::wstring &indexResource,
		const std::vector<DX12CompiledVertexResourceBinding> &vertexResources)
	{
		mChanged |= AppendResourceViewsLocked(
			mDevice, mCommandList, mIaState, indexResource, vertexResources, mReplacement);
	}

	const Bunny::TextureOverrideConfig *FindTargetTextureOverride(
		const Bunny::CommandListTarget &target) const
	{
		return FindTargetTextureOverrideLocked(
			mCommandList, mIaState, target, mVertexCount, mIndexCount, mInstanceCount,
			mFirstVertex, mFirstIndex);
	}

	void RunTextureOverride(
		const Bunny::TextureOverrideConfig &config, int depth, bool includePost)
	{
		if (!mReplacement || depth > 16)
			return;

		if (mExecuteCommands) {
			for (const std::wstring &list : config.commandLists.pre) {
				Bunny::CommandListLinks links;
				links.main.push_back(list);
				RunCommandListLinks(links, depth + 1, false);
			}
		}

		if (!RunCompiledTextureOverridePlan(config)) {
			RunTextureOverrideActions(config, depth, includePost);
		}

		if (mExecuteCommands) {
			for (const std::wstring &list : config.commandLists.main) {
				Bunny::CommandListLinks links;
				links.main.push_back(list);
				RunCommandListLinks(links, depth + 1, false);
			}

			if (includePost) {
				for (const std::wstring &list : config.commandLists.post) {
					Bunny::CommandListLinks links;
					links.main.push_back(list);
					RunCommandListLinks(links, depth + 1, true);
				}
			}
		}
	}

	bool RunCompiledTextureOverridePlan(const Bunny::TextureOverrideConfig &config)
	{
		auto sectionIdIt = gTextureOverrideSectionIds.find(config.section);
		if (sectionIdIt == gTextureOverrideSectionIds.end() || !sectionIdIt->second)
			return false;
		auto planIt = gCompiledTextureOverridePlans.find(sectionIdIt->second);
		if (planIt == gCompiledTextureOverridePlans.end())
			return false;
		const DX12CompiledTextureOverridePlan &plan = planIt->second;
		if (plan.hasUnsupportedCommandActions)
			return false;

		AppendResourceViews(plan.indexResource, plan.vertexResources);
		const bool executeActions = mExecuteCommands || mExecuteDrawActions;
		if (executeActions && plan.handlingSkip)
			SetSkip();
		if (executeActions) {
			for (const DX12ModIaReplacement::DrawCall &draw : plan.draws) {
				mReplacement->draws.push_back(draw);
				MarkChanged();
			}
			for (const DX12ModIaReplacement::DispatchCall &dispatch : plan.dispatches) {
				mReplacement->dispatches.push_back(dispatch);
				MarkChanged();
			}
		}
		return true;
	}

	bool RunCompiledCommandListPlan(const std::wstring &name)
	{
		if (!EnterCommandList(name))
			return true;
		auto planIt = gCompiledCommandListPlans.find(name);
		if (planIt == gCompiledCommandListPlans.end() || planIt->second.unsupported) {
			LeaveCommandList();
			return false;
		}
		ExecuteCompiledCommandListPlan(planIt->second);
		LeaveCommandList();
		return true;
	}

	void ExecuteCompiledCommandListPlan(const DX12CompiledCommandListPlan &plan)
	{
		AppendResourceViews(plan.indexResource, plan.vertexResources);
		if (plan.handlingSkip)
			SetSkip();
		for (const DX12ModIaReplacement::DrawCall &draw : plan.draws) {
			mReplacement->draws.push_back(draw);
			MarkChanged();
		}
		for (const DX12ModIaReplacement::DispatchCall &dispatch : plan.dispatches) {
			mReplacement->dispatches.push_back(dispatch);
			MarkChanged();
		}
	}

	void RunTextureOverrideActions(
		const Bunny::TextureOverrideConfig &config, int depth, bool includePost)
	{
		RunActions(config.actions, depth, includePost);
	}

	void RunCommandList(
		const Bunny::CommandListConfig &commandList, int depth, bool includePost)
	{
		if (!mReplacement || depth > 16)
			return;
		if (RunCompiledCommandListPlan(commandList.section))
			return;
		if (!EnterCommandList(commandList.section))
			return;
		for (const Bunny::CommandListAction &action : commandList.actions) {
			switch (action.kind) {
			case Bunny::CommandListActionKind::Run: {
				auto it = gCommandLists.find(action.commandList);
				if (it != gCommandLists.end())
					RunCommandList(it->second, depth + 1, includePost);
				break;
			}
			case Bunny::CommandListActionKind::CheckTextureOverride: {
				const Bunny::TextureOverrideConfig *config =
					FindTargetTextureOverride(action.target);
				if (config)
					RunTextureOverride(*config, depth + 1, includePost);
				break;
			}
			case Bunny::CommandListActionKind::HandlingSkip:
				SetSkip();
				break;
			case Bunny::CommandListActionKind::SetIndexBuffer:
				AppendResourceViews(action.resource, std::vector<DX12CompiledVertexResourceBinding>());
				break;
			case Bunny::CommandListActionKind::SetVertexBuffer:
				AppendResourceViews(
					L"", std::vector<DX12CompiledVertexResourceBinding>{
						{ action.target.slot, action.resource }});
				break;
			case Bunny::CommandListActionKind::Draw:
			case Bunny::CommandListActionKind::DrawIndexed:
			case Bunny::CommandListActionKind::DrawFromCaller:
			case Bunny::CommandListActionKind::DrawIndexedFromCaller:
				AppendDraw(action);
				break;
			case Bunny::CommandListActionKind::Dispatch:
				AppendDispatch(action);
				break;
			}
		}
		LeaveCommandList();
	}

	void RunActions(
		const std::vector<Bunny::CommandListAction> &actions, int depth, bool includePost)
	{
		if (!mReplacement || depth > 16)
			return;

		const bool executeActions = mExecuteCommands || mExecuteDrawActions;
		for (const Bunny::CommandListAction &action : actions) {
			switch (action.kind) {
			case Bunny::CommandListActionKind::Run: {
				if (!mExecuteCommands)
					break;
				auto it = gCommandLists.find(action.commandList);
				if (it != gCommandLists.end())
					RunCommandList(it->second, depth + 1, includePost);
				break;
			}
			case Bunny::CommandListActionKind::CheckTextureOverride: {
				if (!mExecuteCommands)
					break;
				const Bunny::TextureOverrideConfig *matchedConfig =
					FindTargetTextureOverride(action.target);
				if (matchedConfig)
					RunTextureOverride(*matchedConfig, depth + 1, includePost);
				break;
			}
			case Bunny::CommandListActionKind::HandlingSkip:
				if (executeActions)
					SetSkip();
				break;
			case Bunny::CommandListActionKind::SetIndexBuffer:
				AppendResourceViews(action.resource, std::vector<DX12CompiledVertexResourceBinding>());
				break;
			case Bunny::CommandListActionKind::SetVertexBuffer:
				AppendResourceViews(
					L"", std::vector<DX12CompiledVertexResourceBinding>{
						{ action.target.slot, action.resource }});
				break;
			case Bunny::CommandListActionKind::Draw:
			case Bunny::CommandListActionKind::DrawIndexed:
			case Bunny::CommandListActionKind::DrawFromCaller:
			case Bunny::CommandListActionKind::DrawIndexedFromCaller:
				if (executeActions)
					AppendDraw(action);
				break;
			case Bunny::CommandListActionKind::Dispatch:
				if (executeActions)
					AppendDispatch(action);
				break;
			}
		}
	}

	void RunCommandListLinks(
		const Bunny::CommandListLinks &links, int depth, bool includePost)
	{
		if (depth > 16)
			return;

		for (const std::wstring &list : links.pre) {
			if (!CommandListRuntimeEffectLocked(list))
				continue;
			if (RunCompiledCommandListPlan(list))
				continue;
			auto it = gCommandLists.find(list);
			if (it != gCommandLists.end())
				RunCommandList(it->second, depth + 1, false);
		}
		for (const std::wstring &list : links.main) {
			if (!CommandListRuntimeEffectLocked(list))
				continue;
			if (RunCompiledCommandListPlan(list))
				continue;
			auto it = gCommandLists.find(list);
			if (it != gCommandLists.end())
				RunCommandList(it->second, depth + 1, includePost);
		}
		if (includePost) {
			for (const std::wstring &list : links.post) {
				if (!CommandListRuntimeEffectLocked(list))
					continue;
				if (RunCompiledCommandListPlan(list))
					continue;
				auto it = gCommandLists.find(list);
				if (it != gCommandLists.end())
					RunCommandList(it->second, depth + 1, true);
			}
		}
	}

	bool EnterCommandList(const std::wstring &name)
	{
		if (name.empty())
			return true;
		if (std::find(mCommandListStack.begin(), mCommandListStack.end(), name) ==
		    mCommandListStack.end()) {
			mCommandListStack.push_back(name);
			return true;
		}
#if defined(_DEBUG)
		DX12LogDebugJsonFunc("DX12CommandListCycleSuppressed",
			"\"section\":\"%S\",\"depth\":%zu", name.c_str(),
			mCommandListStack.size());
#endif
		return false;
	}

	void LeaveCommandList()
	{
		if (!mCommandListStack.empty())
			mCommandListStack.pop_back();
	}

	ID3D12Device *mDevice = nullptr;
	ID3D12GraphicsCommandList *mCommandList = nullptr;
	const DX12IaHashState &mIaState;
	uint32_t mVertexCount = 0;
	uint32_t mIndexCount = 0;
	uint32_t mInstanceCount = 0;
	uint32_t mFirstVertex = 0;
	uint32_t mFirstIndex = 0;
	DX12ModIaReplacement *mReplacement = nullptr;
	bool mChanged = false;
	bool mExecuteCommands = true;
	bool mExecuteDrawActions = true;
	std::vector<std::wstring> mCommandListStack;
};

static bool RunNamedCommandListLocked(
	ID3D12GraphicsCommandList *commandList,
	const std::wstring &name,
	DX12ModIaReplacement *replacement)
{
	if (!commandList || !replacement)
		return false;
	if (!CommandListRuntimeEffectLocked(name))
		return false;

	auto it = gCommandLists.find(name);
	if (it == gCommandLists.end())
		return false;

	ID3D12Device *device = nullptr;
	if (FAILED(commandList->GetDevice(IID_PPV_ARGS(&device))) || !device)
		return false;

	DX12IaHashState emptyIa = EmptyIaHashState();
	DX12CommandListExecutor executor(
		device, commandList, emptyIa, 0, 0, 0, 0, 0, replacement);
	executor.RunCommandListLinks(Bunny::CommandListLinks{ {}, { name }, {} }, true);
	device->Release();
	return executor.Changed();
}
